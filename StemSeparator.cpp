#include "StemSeparator.h"

// ONNX Runtime C++ API (same bundled runtime as BeatThisAnalyzer).
#include <onnxruntime_cxx_api.h>

#if defined(__APPLE__)
 #include <coreml_provider_factory.h>
 #include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

//==============================================================================
namespace
{
    // Intra-op thread count for the offline htdemucs pass.
    //
    // On heterogeneous Apple Silicon (M4: 4 performance + 6 efficiency cores)
    // oversubscribing all 10 logical cores is counter-productive for this
    // conv/matmul/attention graph: ORT synchronises at every parallel-region
    // barrier, so the 4 fast P-core threads finish their slice and then stall
    // waiting on 6 E-core threads running at ~1/3 the speed — plus QoS migration
    // and cache thrash. Pinning to the performance-core count is the documented
    // best practice for compute-bound ML. Override with CUE_STEM_THREADS to A/B
    // sweep without a rebuild.
    int chooseIntraOpThreads()
    {
        if (const char* env = std::getenv ("CUE_STEM_THREADS"))
        {
            const int n = std::atoi (env);
            if (n >= 1)
                return juce::jlimit (1, 32, n);
        }

       #if defined(__APPLE__)
        int    perfCores = 0;
        size_t sz        = sizeof (perfCores);
        if (sysctlbyname ("hw.perflevel0.logicalcpu", &perfCores, &sz, nullptr, 0) == 0
            && perfCores >= 1)
            return juce::jlimit (1, 32, perfCores);
       #endif

        return (int) juce::jlimit (1u, 16u, std::thread::hardware_concurrency());
    }

    // Segment overlap for the triangular overlap-add. Lower = fewer inferences
    // (faster) but more boundary artifacts at each ~7.8 s segment seam. Override
    // with CUE_STEM_OVERLAP (clamped [0, 0.5]) to dial in by ear without a rebuild.
    double resolveOverlap (double fallback)
    {
        if (const char* env = std::getenv ("CUE_STEM_OVERLAP"))
        {
            const double v = std::atof (env);
            if (v >= 0.0)
                return juce::jlimit (0.0, 0.5, v);
        }
        return fallback;
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

    const std::string p = path.toStdString();

    // Prefer the CoreML-accelerated options; fall back to CPU if CoreML can't
    // create the session for this graph.
    if (accelOpts != nullptr)
    {
        try
        {
            m->session = std::make_unique<Ort::Session> (env, p.c_str(), *accelOpts);
        }
        catch (const Ort::Exception& e)
        {
            juce::Logger::writeToLog ("StemSeparator: " + juce::String (label)
                                      + " CoreML session failed, retrying on CPU: "
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
        ortOptions = std::make_unique<Ort::SessionOptions>();

        // Offline pass on a single below-realtime thread. On Apple Silicon this
        // defaults to the performance-core count (see chooseIntraOpThreads); other
        // platforms use all logical cores capped at 16. Tune via CUE_STEM_THREADS.
        const int nThreads = chooseIntraOpThreads();
        ortOptions->SetIntraOpNumThreads (nThreads);
        ortOptions->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        juce::Logger::writeToLog ("StemSeparator: intra-op threads = " + juce::String (nThreads));

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
                ortOptionsCoreML = std::make_unique<Ort::SessionOptions> (ortOptions->Clone());
                Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_CoreML (
                    *ortOptionsCoreML, COREML_FLAG_USE_NONE));
                juce::Logger::writeToLog ("StemSeparator: CoreML execution provider enabled (CUE_ENABLE_COREML)");
            }
            catch (const Ort::Exception& e)
            {
                ortOptionsCoreML.reset();
                juce::Logger::writeToLog ("StemSeparator: CoreML EP unavailable, using CPU: "
                                          + juce::String::fromUTF8 (e.what()));
            }
        }
        else
        {
            juce::Logger::writeToLog ("StemSeparator: using CPU (CoreML off by default; set CUE_ENABLE_COREML=1 to try)");
        }
#endif

        Ort::SessionOptions* accel = ortOptionsCoreML.get();
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

    const double ratio  = srcRate / dstRate; // input samples consumed per output sample
    const int    numOut = juce::jmax (1, (int) std::ceil ((double) numIn * dstRate / srcRate));

    juce::AudioBuffer<float> out (numCh, numOut);
    out.clear();
    for (int ch = 0; ch < numCh; ++ch)
    {
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process (ratio, src.getReadPointer (ch), out.getWritePointer (ch), numOut);
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
    const double overlap = resolveOverlap (kOverlap);

    const int seg    = kSegmentSamples;
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

    int numSeg = 0;
    for (int o = 0; o < L; o += stride) ++numSeg;
    int segDone = 0;

    for (int offset = 0; offset < L; offset += stride)
    {
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
            outs = model.session->Run (Ort::RunOptions { nullptr },
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
    if (srcCh <= 0 || srcLen <= 0 || sampleRate <= 0.0)
        return result;

    if (progress) progress (0.0f);

    // 1. Resample to 44.1 kHz and force stereo for the model.
    auto mix44 = toStereo (resample (buffer, sampleRate, kModelSampleRate));
    if (mix44.getNumSamples() <= 0)
        return result;
    if (shouldAbort && shouldAbort())
        return result;

    // 2. One segmented pass over the base model yields all three stems at once.
    auto stems = runModel (*stemModel, mix44, progress, shouldAbort);
    if (stems.drums.getNumSamples() <= 0)
        return result; // aborted or failed → invalid result, caller keeps the original

    // 3. Resample each stem back to the source rate and conform to source shape.
    result.drums  = conform (resample (stems.drums,  kModelSampleRate, sampleRate), srcCh, srcLen);
    result.bass   = conform (resample (stems.bass,   kModelSampleRate, sampleRate), srcCh, srcLen);
    result.vocals = conform (resample (stems.vocals, kModelSampleRate, sampleRate), srcCh, srcLen);
    result.valid  = true;

    if (progress) progress (1.0f);
    return result;
}
