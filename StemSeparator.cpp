#include "StemSeparator.h"
#include "StemProcessingSettings.h"

// ONNX Runtime C++ API (same bundled runtime as BeatThisAnalyzer).
#include <onnxruntime_cxx_api.h>
#include <onnxruntime_session_options_config_keys.h>

#if defined(__APPLE__)
 #include <coreml_provider_factory.h>
 #include <sys/sysctl.h>
#elif defined(_WIN32)
 #include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

//==============================================================================
namespace
{
    // Limit offline inference so an older Mac is still usable during separation.
    // Explicit thread counts are a budget, not CPU affinity. CUE_STEM_THREADS
    // remains available for machine-specific benchmarking/support.
    int chooseIntraOpThreads()
    {
        if (const char* env = std::getenv ("CUE_STEM_THREADS"))
        {
            char* end = nullptr;
            const double n = std::strtod (env, &end);
            if (end != env && *end == '\0' && std::isfinite (n) && n >= 1.0 && std::floor (n) == n)
                return (int) juce::jlimit (1.0, 32.0, n);
        }

        int performanceCores = 0;
       #if defined(__APPLE__)
        size_t sz = sizeof (performanceCores);
        if (sysctlbyname ("hw.perflevel0.logicalcpu", &performanceCores, &sz, nullptr, 0) != 0)
            performanceCores = 0; // Intel Macs and unavailable sysctl: use bounded fallback.
       #endif

        return cuesampler::StemProcessingSettings::defaultIntraOpThreads (
            std::thread::hardware_concurrency(), performanceCores);
    }


}

//==============================================================================
// The loaded model: its Ort::Session plus the I/O names queried from the graph.
// Kept in the .cpp so the ORT headers stay out of StemSeparator.h.
struct StemSeparator::Model
{
    std::unique_ptr<Ort::Session> session;
    std::string                   inputName;
    std::vector<std::string>      outputNames;
    std::unique_ptr<Ort::RunOptions> runOpts;
    bool                          ready = false;
};

std::unique_ptr<StemSeparator::Model> StemSeparator::loadModel (Ort::Env& env,
                                                                Ort::SessionOptions* accelOpts,
                                                                Ort::SessionOptions& cpuOpts,
                                                                const juce::String& path,
                                                                const char* label)
{
    auto m = std::make_unique<Model>();

    if (path.isEmpty() || ! juce::File (path).existsAsFile())
    {
        juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                                  + " model not found: " + path);
        return m; // ready == false
    }

    // ONNX takes the model path as ORTCHAR_T*: wchar_t* on Windows, char*
    // elsewhere. Build the matching owned string so p.c_str() has the right
    // type for both Ort::Session constructions below.
   #ifdef _WIN32
    const std::wstring p (path.toWideCharPointer());
   #else
    const std::string p = path.toStdString();
   #endif

    // Validate that a session can actually RUN this graph (one silent forward
    // pass), not merely be created. An accelerator EP can build the session fine
    // and then fail at run time — DirectML on a memory-constrained GPU throws
    // E_OUTOFMEMORY during graph fusion or hangs the device (TDR); CoreML has
    // produced NaNs. Probing at load time lets us fall back to CPU here, instead
    // of silently returning no stems on every separation.
    auto canRun = [&] (Ort::Session& s) -> bool
    {
        try
        {
            Ort::AllocatorWithDefaultOptions a;
            const std::string inName = s.GetInputNameAllocated (0, a).get();
            auto shape = s.GetInputTypeInfo (0).GetTensorTypeAndShapeInfo().GetShape();
            size_t n = 1;
            for (auto& d : shape) { if (d < 0) d = 1; n *= (size_t) d; }

            std::vector<float> zeros (n, 0.0f);
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value in = Ort::Value::CreateTensor<float> (mem, zeros.data(), zeros.size(),
                                                             shape.data(), shape.size());

            std::vector<std::string> outOwned;
            for (size_t i = 0, e = s.GetOutputCount(); i < e; ++i)
                outOwned.emplace_back (s.GetOutputNameAllocated (i, a).get());
            std::vector<const char*> outPtrs;
            for (auto& o : outOwned) outPtrs.push_back (o.c_str());

            const char* inPtr = inName.c_str();
            auto outs = s.Run (Ort::RunOptions { nullptr }, &inPtr, &in, 1,
                               outPtrs.data(), outPtrs.size());
            return ! outs.empty();
        }
        catch (const Ort::Exception& e)
        {
            juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                + " accelerator probe failed, falling back to CPU: "
                + juce::String::fromUTF8 (e.what()));
            return false;
        }
    };

    // Prefer the accelerated options (DirectML on Windows / CoreML on macOS); fall
    // back to CPU if the accelerator can't create OR run the session for this graph.
    bool boundAccel = false;
    if (accelOpts != nullptr)
    {
        try
        {
            m->session = std::make_unique<Ort::Session> (env, p.c_str(), *accelOpts);
            if (canRun (*m->session))
                boundAccel = true;
            else
                m->session.reset(); // probe failed → fall through to CPU below
        }
        catch (const Ort::Exception& e)
        {
            juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                                      + " accelerated session failed, retrying on CPU: "
                                      + juce::String::fromUTF8 (e.what()));
            m->session.reset();
        }
    }

    try
    {
        if (m->session == nullptr)
            m->session = std::make_unique<Ort::Session> (env, p.c_str(), cpuOpts);

        Ort::AllocatorWithDefaultOptions alloc;
        m->inputName = m->session->GetInputNameAllocated (0, alloc).get();
        const size_t numOut = m->session->GetOutputCount();
        for (size_t i = 0; i < numOut; ++i)
            m->outputNames.emplace_back (m->session->GetOutputNameAllocated (i, alloc).get());

        m->ready = ! m->outputNames.empty();
        if (m->ready)
        {
            m->runOpts = std::make_unique<Ort::RunOptions>();

            // Log the input rank/dims so it's clear whether the length axis is
            // dynamic (-1) — i.e. whether CUE_STEM_SEGMENT can shrink it to fit a
            // memory-constrained accelerator.
            const auto inShape = m->session->GetInputTypeInfo (0)
                                     .GetTensorTypeAndShapeInfo().GetShape();
            juce::String dims;
            for (const auto d : inShape) dims << juce::String (d) << " ";
            juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                + " session bound to " + juce::String (boundAccel ? "GPU/accelerator EP" : "CPU EP")
                + ", input dims [" + dims.trim() + "]  (-1 = dynamic)");
        }
    }
    catch (const Ort::Exception& e)
    {
        juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                                  + " load failed: " + juce::String::fromUTF8 (e.what()));
        m->session.reset();
    }

    return m;
}

//==============================================================================
StemSeparator::StemSeparator (const ModelPaths& paths)
{
    try
    {
        ortEnv     = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "StemSeparator");
        // Configure diagnostics before any model/session work, off the audio thread.
        ortEnv->DisableTelemetryEvents();
        ortOptions = std::make_unique<Ort::SessionOptions>();

        // Bound inference workers on every platform, including Intel Macs.
        const int nThreads = chooseIntraOpThreads();
        ortOptions->SetIntraOpNumThreads (nThreads);
        ortOptions->SetInterOpNumThreads (1);
        ortOptions->SetExecutionMode (ORT_SEQUENTIAL);
        ortOptions->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
       #if defined(__APPLE__)
        // ORT 1.18 packs a new memory-pattern buffer after the first window,
        // while its arena retains the first window's allocations. HTDemucs
        // consequently grows from ~2.8 to ~4.8 GiB on the second window. Keep
        // normal arena reuse without that extra packed buffer (measured ~1%
        // wall-time cost, identical output across repeated windows).
        ortOptions->DisableMemPattern();
       #endif
        // Idle ORT workers otherwise spin between operators at normal priority,
        // competing with UI rendering even though the caller is a background job.
        ortOptions->AddConfigEntry (kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");
        ortOptions->AddConfigEntry (kOrtSessionOptionsConfigAllowInterOpSpinning, "0");
        juce::Logger::writeToLog ("StemSeparator: intra-op threads = " + juce::String (nThreads)
            + ", worker spinning disabled");

#if defined(__APPLE__)
        // CoreML is OFF by default for the stem models — pure CPU. Benchmarked
        // 2026-06-18 on Apple Silicon (htdemucs_ft, 18s clip): htdemucs's
        // full-segment tensors exceed CoreML's 16384-dim cap, so ORT splits the
        // graph into ~106 CoreML/CPU partitions. That path produced NaN stems AND
        // ran ~25x slower (561s vs 22s) with a 42s first-load compile, whereas pure
        // CPU is correct and ~0.8x realtime. Set CUE_ENABLE_COREML=1 to opt back in
        // (e.g. to re-test on a newer onnxruntime/CoreML). loadModel also retries
        // per-model on CPU if a CoreML session fails to build.
        if (std::getenv ("CUE_ENABLE_COREML") != nullptr)
        {
            try
            {
                ortOptionsAccel = std::make_unique<Ort::SessionOptions> (ortOptions->Clone());
                Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_CoreML (
                    *ortOptionsAccel, COREML_FLAG_USE_NONE));
                juce::Logger::writeToLog ("StemSeparator: CoreML execution provider enabled (CUE_ENABLE_COREML)");
            }
            catch (const Ort::Exception& e)
            {
                ortOptionsAccel.reset();
                juce::Logger::writeToLog ("StemSeparator: CoreML EP unavailable, using CPU: "
                                          + juce::String::fromUTF8 (e.what()));
            }
        }
        else
        {
            juce::Logger::writeToLog ("StemSeparator: using CPU (CoreML off by default; set CUE_ENABLE_COREML=1 to try)");
        }
#endif

#if defined(_WIN32)
        // DirectML shares the display GPU with the editor and Windows compositor.
        // HTDemucs can monopolise it throughout processing or trigger TDR; a
        // successful load-time probe does not guarantee responsive rendering.
        // Use CPU by default. CUE_ENABLE_DIRECTML=1 explicitly opts into GPU
        // inference; the existing CUE_DISABLE_DIRECTML override takes precedence.
        // The DML EP has two hard
        // requirements: memory pattern OFF and SEQUENTIAL execution mode (it does
        // not support ORT's parallel mem-pattern planner). loadModel still retries
        // on CPU if the DML session fails to build, and runModel guards against
        // non-finite GPU output (an earlier CoreML attempt on this model NaN'd).
        const auto* enableDirectML = std::getenv ("CUE_ENABLE_DIRECTML");
        if (enableDirectML != nullptr && std::string (enableDirectML) == "1"
            && std::getenv ("CUE_DISABLE_DIRECTML") == nullptr)
        {
            try
            {
                ortOptionsAccel = std::make_unique<Ort::SessionOptions> (ortOptions->Clone());
                ortOptionsAccel->DisableMemPattern();
                ortOptionsAccel->SetExecutionMode (ORT_SEQUENTIAL);
                // Full optimization by default: fastest on GPUs that have the memory
                // budget for this model's fixed 7.8 s segment. On a budget-constrained
                // GPU it fails cleanly and instantly with E_OUTOFMEMORY during graph
                // fusion (caught by the load-time probe → CPU fallback). Lowering the
                // level keeps the graph as smaller ops so ORT can reuse buffers (less
                // peak memory) but can run long enough per dispatch to trip the GPU
                // watchdog (TDR, a disruptive device reset) — so it's opt-in only.
                // Tune with CUE_STEM_DML_OPT (0=disable_all, 1=basic, 2=extended,
                // 3=enable_all).
                {
                    auto lvl = GraphOptimizationLevel::ORT_ENABLE_ALL;
                    if (const char* o = std::getenv ("CUE_STEM_DML_OPT"))
                        switch (juce::jlimit (0, 3, std::atoi (o)))
                        {
                            case 0:  lvl = GraphOptimizationLevel::ORT_DISABLE_ALL;     break;
                            case 1:  lvl = GraphOptimizationLevel::ORT_ENABLE_BASIC;    break;
                            case 2:  lvl = GraphOptimizationLevel::ORT_ENABLE_EXTENDED; break;
                            default: lvl = GraphOptimizationLevel::ORT_ENABLE_ALL;      break;
                        }
                    ortOptionsAccel->SetGraphOptimizationLevel (lvl);
                }
                Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_DML (*ortOptionsAccel, 0));
                juce::Logger::writeToLog ("StemSeparator: DirectML execution provider enabled (device 0)");
            }
            catch (const Ort::Exception& e)
            {
                ortOptionsAccel.reset();
                juce::Logger::writeToLog ("StemSeparator: DirectML EP unavailable, using CPU: "
                                          + juce::String::fromUTF8 (e.what()));
            }
        }
        else
        {
            juce::Logger::writeToLog ("StemSeparator: using CPU (DirectML opt-in via CUE_ENABLE_DIRECTML=1; CUE_DISABLE_DIRECTML overrides)");
        }
#endif

        Ort::SessionOptions* accel = ortOptionsAccel.get();
        stemModel = loadModel (*ortEnv, accel, *ortOptions, paths.model, "htdemucs");

        sessionReady.store (stemModel->ready);

        juce::Logger::writeToLog ("StemSeparator: ready="
            + juce::String (stemModel->ready ? "YES" : "NO"));

        if (stemModel->ready)
            juce::Logger::writeToLog ("StemSeparator: input '" + juce::String (stemModel->inputName)
                + "', " + juce::String ((int) stemModel->outputNames.size()) + " output(s)");
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog ("StemSeparator: init error: " + juce::String::fromUTF8 (e.what()));
        sessionReady.store (false);
    }
}

StemSeparator::~StemSeparator() = default;

//==============================================================================
juce::AudioBuffer<float> StemSeparator::resample (const juce::AudioBuffer<float>& src,
                                                  double srcRate, double dstRate)
{
    const int numCh  = src.getNumChannels();
    const int numIn  = src.getNumSamples();
    if (numCh <= 0 || numIn <= 0)
        return {};

    if (srcRate <= 0.0 || dstRate <= 0.0 || juce::approximatelyEqual (srcRate, dstRate))
    {
        juce::AudioBuffer<float> copy (numCh, numIn);
        for (int ch = 0; ch < numCh; ++ch)
            copy.copyFrom (ch, 0, src, ch, 0, numIn);
        return copy;
    }

    const double ratio = srcRate / dstRate;
    const int numOut = juce::jmax (1, (int) std::ceil ((double) numIn / ratio));

    // Offline, zero-phase band-limited resampling. The cutoff follows the lower
    // Nyquist limit; Lagrange interpolation alone did not reject frequencies
    // above that limit when downsampling. A Blackman-windowed sinc gives a
    // smooth transition while symmetric reads preserve absolute sample timing.
    const double cutoff = 0.95 * juce::jmin (1.0, 1.0 / ratio);
    const int radius = (int) std::ceil (32.0 / cutoff);
    const int taps = radius * 2 + 1;
    constexpr int phases = 1024;
    const double pi = juce::MathConstants<double>::pi;
    std::vector<float> kernels ((size_t) (phases + 1) * (size_t) taps);
    for (int phase = 0; phase <= phases; ++phase)
    {
        auto* kernel = kernels.data() + (size_t) phase * (size_t) taps;
        const double fraction = (double) phase / phases;
        double sum = 0.0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double x = (tap - radius) - fraction;
            const double t = x * cutoff;
            const double window = std::abs (t) <= 32.0
                ? 0.42 + 0.5 * std::cos (pi * t / 32.0) + 0.08 * std::cos (2.0 * pi * t / 32.0)
                : 0.0;
            const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (pi * t) / (pi * t);
            kernel[tap] = (float) (cutoff * sinc * window);
            sum += kernel[tap];
        }
        for (int tap = 0; tap < taps; ++tap) kernel[tap] /= (float) sum;
    }

    juce::AudioBuffer<float> out (numCh, numOut);
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto* input = src.getReadPointer (ch);
        auto* output = out.getWritePointer (ch);
        for (int i = 0; i < numOut; ++i)
        {
            const double position = (double) i * ratio;
            const int centre = (int) std::floor (position);
            const double phasePosition = (position - centre) * phases;
            const int phase = juce::jlimit (0, phases - 1, (int) phasePosition);
            const float fraction = (float) (phasePosition - phase);
            const auto* a = kernels.data() + (size_t) phase * (size_t) taps;
            const auto* b = a + taps;
            double sample = 0.0;
            for (int tap = 0; tap < taps; ++tap)
            {
                // Extend the edge value; even one-frame buffers remain bounded
                // and constant signals stay constant through the boundary.
                const int index = juce::jlimit (0, numIn - 1, centre + tap - radius);
                sample += (double) input[index] * (a[tap] + fraction * (b[tap] - a[tap]));
            }
            output[i] = (float) sample;
        }
    }
    return out;
}

juce::AudioBuffer<float> StemSeparator::toStereo (const juce::AudioBuffer<float>& src)
{
    const int n = src.getNumSamples();
    juce::AudioBuffer<float> out (kModelChannels, juce::jmax (0, n));
    out.clear();
    if (src.getNumChannels() <= 0 || n <= 0)
        return out;

    if (src.getNumChannels() == 1)
    {
        out.copyFrom (0, 0, src, 0, 0, n);
        out.copyFrom (1, 0, src, 0, 0, n);
    }
    else
    {
        out.copyFrom (0, 0, src, 0, 0, n);
        out.copyFrom (1, 0, src, 1, 0, n);
    }
    return out;
}

juce::AudioBuffer<float> StemSeparator::conform (const juce::AudioBuffer<float>& src,
                                                 int dstChannels, int dstLen)
{
    juce::AudioBuffer<float> out (juce::jmax (1, dstChannels), juce::jmax (0, dstLen));
    out.clear();

    const int n     = juce::jmin (dstLen, src.getNumSamples());
    const int srcCh = src.getNumChannels();
    if (n <= 0 || srcCh <= 0)
        return out;

    if (dstChannels == 1)
    {
        float* d = out.getWritePointer (0);
        if (srcCh == 1)
            for (int i = 0; i < n; ++i) d[i] = src.getReadPointer (0)[i];
        else
            for (int i = 0; i < n; ++i)
                d[i] = 0.5f * (src.getReadPointer (0)[i] + src.getReadPointer (1)[i]);
    }
    else
    {
        for (int ch = 0; ch < dstChannels; ++ch)
            out.copyFrom (ch, 0, src, juce::jmin (ch, srcCh - 1), 0, n);
    }
    return out;
}

//==============================================================================
StemSeparator::Stems44 StemSeparator::runModel (const Model& model,
                                                const juce::AudioBuffer<float>& mix44,
                                                const std::function<void(float)>& progress,
                                                const std::function<bool()>& shouldAbort) const
{
    Stems44 empty;
    if (! model.ready || model.session == nullptr)
        return empty;

    const int L = mix44.getNumSamples();
    if (L <= 0)
        return empty;

    const double t0      = juce::Time::getMillisecondCounterHiRes();
    const auto settings = cuesampler::StemProcessingSettings::fromEnvironment();
    const double overlap = settings.overlap;

    // The published fp32 graph currently has a fixed T=343980 input. Honour a
    // fixed model dimension even if CUE_STEM_SEGMENT is set; only apply the
    // memory-tuning override to a future export whose time axis is dynamic.
    const auto modelInputShape = model.session->GetInputTypeInfo (0)
                                     .GetTensorTypeAndShapeInfo().GetShape();
    const bool dynamicTimeAxis = modelInputShape.size() >= 3 && modelInputShape[2] < 0;
    const int fixedModelSegment = (modelInputShape.size() >= 3 && modelInputShape[2] > 0)
                                ? (int) modelInputShape[2]
                                : kSegmentSamples;
    const int seg    = dynamicTimeAxis ? settings.requestedSegmentSamples : fixedModelSegment;
    const int stride = juce::jmax (1, (int) std::llround ((double) seg * (1.0 - overlap)));

    // Triangular overlap-add weight (Demucs, transition_power = 1): ramp 1..max
    // then max..1, normalized to a peak of 1.
    std::vector<float> weight ((size_t) seg);
    const int half = seg / 2;
    for (int i = 0; i < seg; ++i)
        weight[(size_t) i] = (float) ((i < half) ? (i + 1) : (seg - i));
    const float wmax = *std::max_element (weight.begin(), weight.end());
    for (auto& w : weight) w /= wmax;

    // One accumulator per kept stem (drums/bass/vocals), all sharing one weight sum.
    constexpr int kNumKept = 3;
    Stems44 stems;
    juce::AudioBuffer<float>* outBufs[kNumKept] = { &stems.drums, &stems.bass, &stems.vocals };
    for (auto* b : outBufs) { b->setSize (kModelChannels, L); b->clear(); }
    std::vector<double> sumW ((size_t) L, 0.0);

    std::vector<float> inFlat ((size_t) kModelChannels * (size_t) seg);

    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 3> inShape { 1, (int64_t) kModelChannels, (int64_t) seg };

    const int numSeg = cuesampler::StemProcessingSettings::windowCount (L, seg, stride);
    int segDone = 0;

    for (int segmentIndex = 0; segmentIndex < numSeg; ++segmentIndex)
    {
        const int offset = segmentIndex * stride;
        if (shouldAbort && shouldAbort())
            return empty;

        const int chunkLen = juce::jmin (seg, L - offset);

        std::fill (inFlat.begin(), inFlat.end(), 0.0f);
        for (int ch = 0; ch < kModelChannels; ++ch)
        {
            const float* srcCh = mix44.getReadPointer (juce::jmin (ch, mix44.getNumChannels() - 1));
            float* dst = inFlat.data() + (size_t) ch * (size_t) seg;
            std::copy (srcCh + offset, srcCh + offset + chunkLen, dst);
        }

        Ort::Value inputTensor = Ort::Value::CreateTensor<float> (
            memInfo, inFlat.data(), inFlat.size(), inShape.data(), inShape.size());

        const char* inName = model.inputName.c_str();
        std::vector<const char*> outNames;
        outNames.reserve (model.outputNames.size());
        for (const auto& s : model.outputNames)
            outNames.push_back (s.c_str());

        std::vector<Ort::Value> outs;
        try
        {
            outs = model.session->Run (*model.runOpts,
                                       &inName, &inputTensor, 1,
                                       outNames.data(), outNames.size());
        }
        catch (const Ort::Exception& e)
        {
            juce::Logger::writeToLog ("StemSeparator: inference error: "
                                      + juce::String::fromUTF8 (e.what()));
            return empty;
        }
        if (outs.empty())
            return empty;

        // The base htdemucs graph returns all four stems as [1, S, C, T]; slice
        // the three we keep in one go. (Also tolerate a [1, C, T] single-stem
        // graph by treating S as 1 — every kept index then clamps to that stem.)
        const auto  info  = outs[0].GetTensorTypeAndShapeInfo();
        const auto  shape = info.GetShape();
        const float* raw  = outs[0].GetTensorData<float>();

        int S = 1, outCh = kModelChannels, T = 0;
        if (shape.size() == 4)      { S = (int) shape[1]; outCh = (int) shape[2]; T = (int) shape[3]; }
        else if (shape.size() == 3) { outCh = (int) shape[1]; T = (int) shape[2]; }
        else
        {
            juce::Logger::writeToLog ("StemSeparator: unexpected output rank "
                                      + juce::String ((int) shape.size()));
            return empty;
        }

        const int useLen = juce::jmin (chunkLen, T);
        for (int k = 0; k < kNumKept; ++k)
        {
            const int    stemIdx    = juce::jlimit (0, S - 1, kKeptStemIndices[k]);
            const size_t stemOffset = (size_t) stemIdx * (size_t) outCh * (size_t) T;
            for (int ch = 0; ch < kModelChannels; ++ch)
            {
                const int    sc    = juce::jmin (ch, outCh - 1);
                const float* segCh = raw + stemOffset + (size_t) sc * (size_t) T;
                float*       dst   = outBufs[k]->getWritePointer (ch) + offset;
                for (int i = 0; i < useLen; ++i)
                    dst[i] += weight[(size_t) i] * segCh[i];
            }
        }
        for (int i = 0; i < useLen; ++i)
            sumW[(size_t) (offset + i)] += (double) weight[(size_t) i];

        ++segDone;
        if (progress)
            progress (juce::jlimit (0.0f, 1.0f, (float) segDone / (float) juce::jmax (1, numSeg)));
    }

    // Normalize each stem by the accumulated overlap weight.
    for (int k = 0; k < kNumKept; ++k)
        for (int ch = 0; ch < kModelChannels; ++ch)
        {
            float* d = outBufs[k]->getWritePointer (ch);
            for (int i = 0; i < L; ++i)
            {
                const double w = sumW[(size_t) i];
                if (w > 1e-8)
                    d[i] = (float) ((double) d[i] / w);
            }
        }

    // Safety net for a misbehaving accelerator EP: an earlier CoreML attempt on
    // this exact model produced all-NaN stems (see constructor notes). If the GPU
    // path returns non-finite audio, bail so the caller keeps the original buffer
    // instead of writing NaN/Inf into the project. (Set CUE_DISABLE_DIRECTML=1 to
    // force the CPU path.)
    for (auto* b : outBufs)
        for (int ch = 0; ch < kModelChannels; ++ch)
        {
            const float* d = b->getReadPointer (ch);
            for (int i = 0; i < L; ++i)
                if (! std::isfinite (d[i]))
                {
                    juce::Logger::writeToLog ("StemSeparator: non-finite output detected — aborting "
                        "(set CUE_DISABLE_DIRECTML=1 to force CPU)");
                    return empty;
                }
        }

    // Field metric: wall-clock vs audio length, so EP/thread/overlap changes are
    // always measurable from the log without external profiling.
    const double elapsedSec = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    const double audioSec   = (double) L / kModelSampleRate;
    const double xRealtime  = elapsedSec > 1e-6 ? audioSec / elapsedSec : 0.0;
    juce::Logger::writeToLog ("StemSeparator: " + juce::String (numSeg) + " seg, overlap "
        + juce::String (overlap, 2) + " — " + juce::String (elapsedSec, 2) + " s wall for "
        + juce::String (audioSec, 1) + " s audio (" + juce::String (xRealtime, 2) + "x realtime, "
        + juce::String (xRealtime > 1e-6 ? 60.0 / xRealtime : 0.0, 1) + " s/min-of-audio)");

    return stems;
}

//==============================================================================
StemSeparator::StemResult StemSeparator::separate (const juce::AudioBuffer<float>& buffer,
                                                   double sampleRate,
                                                   std::function<void(float)> progress,
                                                   std::function<bool()> shouldAbort) const
{
    StemResult result;

    if (! sessionReady.load())
        return result;

    const int srcCh  = buffer.getNumChannels();
    const int srcLen = buffer.getNumSamples();
    if (srcCh <= 0 || srcLen <= 0 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
        return result;

    if (progress) progress (0.0f);

    const bool atModelRate = juce::approximatelyEqual (sampleRate, kModelSampleRate);
    Stems44 stems;
    {
        // Borrow an already-compatible source. Keep conversion storage in this
        // scope so it is released before allocating source-rate output stems.
        juce::AudioBuffer<float> convertedMix;
        const auto* mix44 = &buffer;
        if (srcCh != kModelChannels || ! atModelRate)
        {
            if (srcCh > kModelChannels)
            {
                // Discard unused channels before the expensive resampling pass.
                convertedMix = toStereo (buffer);
                if (! atModelRate)
                    convertedMix = resample (convertedMix, sampleRate, kModelSampleRate);
            }
            else if (atModelRate)
            {
                convertedMix = toStereo (buffer);
            }
            else
            {
                // Resample mono once, then duplicate it for the stereo model.
                convertedMix = resample (buffer, sampleRate, kModelSampleRate);
                if (srcCh != kModelChannels)
                    convertedMix = toStereo (convertedMix);
            }
            mix44 = &convertedMix;
        }
        if (mix44->getNumSamples() <= 0 || (shouldAbort && shouldAbort()))
            return result;

        // One segmented pass over the base model yields all three stems at once.
        stems = runModel (*stemModel, *mix44, progress, shouldAbort);
    }
    if (stems.drums.getNumSamples() <= 0)
        return result; // aborted or failed → invalid result, caller keeps the original

    // Transfer compatible stems directly; resample/conform only when required.
    auto finishStem = [&] (juce::AudioBuffer<float>& stem) -> juce::AudioBuffer<float>
    {
        if (! atModelRate)
            stem = resample (stem, kModelSampleRate, sampleRate);
        if (stem.getNumChannels() == srcCh && stem.getNumSamples() == srcLen)
            return std::move (stem);

        auto output = conform (stem, srcCh, srcLen);
        stem = {};
        return output;
    };
    result.drums  = finishStem (stems.drums);
    result.bass   = finishStem (stems.bass);
    result.vocals = finishStem (stems.vocals);
    result.valid  = true;

    if (progress) progress (1.0f);
    return result;
}

//==============================================================================
void StemSeparator::terminate() const
{
    if (stemModel && stemModel->ready && stemModel->runOpts)
    {
        try {
            stemModel->runOpts->SetTerminate();
        } catch (...) {}
    }
}
