#include "StemSeparator.h"

// ONNX Runtime C++ API (same bundled runtime as BeatThisAnalyzer).
#include <onnxruntime_cxx_api.h>

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

    // Per-inference segment length in samples. The htdemucs graph takes a dynamic
    // length, so a SHORTER segment cuts the activation memory of each forward pass
    // — the lever that lets a memory-constrained accelerator run this graph, since
    // DirectML pre-allocates the whole fused segment up front (a full 7.8 s segment
    // can blow the GPU's memory budget and fail graph fusion with E_OUTOFMEMORY).
    // The cost is more inferences and more overlap-add seams. Override with
    // CUE_STEM_SEGMENT (samples @ 44.1 kHz; clamped [44100, kSegmentSamples], i.e.
    // 1 s .. 7.8 s) to fit the accelerator without a rebuild.
    int resolveSegment (int fallback)
    {
        if (const char* env = std::getenv ("CUE_STEM_SEGMENT"))
        {
            const int n = std::atoi (env);
            if (n >= 44100)
                return juce::jlimit (44100, fallback, n);
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
        // DirectML execution provider — runs the htdemucs graph on any DX12 GPU
        // (NVIDIA/AMD/Intel), ~10x faster than the CPU path (which, in a VM, may
        // see only a handful of cores). ON by default; set CUE_DISABLE_DIRECTML=1
        // to force CPU (headless/CI, or to A/B). The DML EP has two hard
        // requirements: memory pattern OFF and SEQUENTIAL execution mode (it does
        // not support ORT's parallel mem-pattern planner). loadModel still retries
        // on CPU if the DML session fails to build, and runModel guards against
        // non-finite GPU output (an earlier CoreML attempt on this model NaN'd).
        if (std::getenv ("CUE_DISABLE_DIRECTML") == nullptr)
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
            juce::Logger::writeToLog ("StemSeparator: DirectML disabled (CUE_DISABLE_DIRECTML), using CPU");
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

    // The published fp32 graph currently has a fixed T=343980 input. Honour a
    // fixed model dimension even if CUE_STEM_SEGMENT is set; only apply the
    // memory-tuning override to a future export whose time axis is dynamic.
    const auto modelInputShape = model.session->GetInputTypeInfo (0)
                                     .GetTensorTypeAndShapeInfo().GetShape();
    const bool dynamicTimeAxis = modelInputShape.size() >= 3 && modelInputShape[2] < 0;
    const int fixedModelSegment = (modelInputShape.size() >= 3 && modelInputShape[2] > 0)
                                ? (int) modelInputShape[2]
                                : kSegmentSamples;
    const int seg    = dynamicTimeAxis ? resolveSegment (kSegmentSamples) : fixedModelSegment;
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
