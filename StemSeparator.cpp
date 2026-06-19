#include "StemSeparator.h"

// ONNX Runtime C++ API (same bundled runtime as BeatThisAnalyzer).
#include <onnxruntime_cxx_api.h>

#if defined(__APPLE__)
 #include <coreml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

//==============================================================================
// One loaded FT specialist: its Ort::Session plus the I/O names queried from the
// graph. Kept in the .cpp so the ORT headers stay out of StemSeparator.h.
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

        // Offline pass: let it use several cores, but cap so it doesn't starve the
        // system. Phase 3 runs separate() on a single below-realtime thread.
        const int nThreads = (int) juce::jlimit (1u, 8u, std::thread::hardware_concurrency());
        ortOptions->SetIntraOpNumThreads (nThreads);
        ortOptions->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

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
        drumsModel  = loadModel (*ortEnv, accel, *ortOptions, paths.drums,  "drums");
        bassModel   = loadModel (*ortEnv, accel, *ortOptions, paths.bass,   "bass");
        vocalsModel = loadModel (*ortEnv, accel, *ortOptions, paths.vocals, "vocals");

        const bool allReady = drumsModel->ready && bassModel->ready && vocalsModel->ready;
        sessionReady.store (allReady);

        juce::Logger::writeToLog ("StemSeparator: ready=" + juce::String (allReady ? "YES" : "NO")
            + " (drums=" + juce::String (drumsModel->ready  ? "Y" : "N")
            + " bass="   + juce::String (bassModel->ready   ? "Y" : "N")
            + " vocals=" + juce::String (vocalsModel->ready ? "Y" : "N") + ")");

        if (allReady)
            juce::Logger::writeToLog ("StemSeparator: input '" + juce::String (drumsModel->inputName)
                + "', " + juce::String ((int) drumsModel->outputNames.size()) + " output(s)");
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
juce::AudioBuffer<float> StemSeparator::runModel (const Model& model,
                                                  const juce::AudioBuffer<float>& mix44,
                                                  int specialtyStemIndex,
                                                  const std::function<void(float)>& progress,
                                                  const std::function<bool()>& shouldAbort) const
{
    if (! model.ready || model.session == nullptr)
        return {};

    const int L = mix44.getNumSamples();
    if (L <= 0)
        return {};

    const int seg    = kSegmentSamples;
    const int stride = juce::jmax (1, (int) std::llround ((double) seg * (1.0 - kOverlap)));

    // Triangular overlap-add weight (Demucs, transition_power = 1): ramp 1..max
    // then max..1, normalized to a peak of 1.
    std::vector<float> weight ((size_t) seg);
    const int half = seg / 2;
    for (int i = 0; i < seg; ++i)
        weight[(size_t) i] = (float) ((i < half) ? (i + 1) : (seg - i));
    const float wmax = *std::max_element (weight.begin(), weight.end());
    for (auto& w : weight) w /= wmax;

    juce::AudioBuffer<float> out (kModelChannels, L);
    out.clear();
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
            return {};

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
            return {};
        }
        if (outs.empty())
            return {};

        // Slice the specialty stem from the output tensor. Handles both the
        // single-stem export ([1, C, T]) and an all-stems export ([1, S, C, T]).
        const auto  info  = outs[0].GetTensorTypeAndShapeInfo();
        const auto  shape = info.GetShape();
        const float* raw  = outs[0].GetTensorData<float>();

        int T = 0, outCh = kModelChannels;
        size_t stemOffset = 0;
        if (shape.size() == 3)
        {
            outCh = (int) shape[1];
            T     = (int) shape[2];
        }
        else if (shape.size() == 4)
        {
            const int S = (int) shape[1];
            outCh = (int) shape[2];
            T     = (int) shape[3];
            const int stemIdx = juce::jlimit (0, S - 1, specialtyStemIndex);
            stemOffset = (size_t) stemIdx * (size_t) outCh * (size_t) T;
        }
        else
        {
            juce::Logger::writeToLog ("StemSeparator: unexpected output rank "
                                      + juce::String ((int) shape.size()));
            return {};
        }

        const int useLen = juce::jmin (chunkLen, T);
        for (int ch = 0; ch < kModelChannels; ++ch)
        {
            const int    sc    = juce::jmin (ch, outCh - 1);
            const float* segCh = raw + stemOffset + (size_t) sc * (size_t) T;
            float*       dst   = out.getWritePointer (ch) + offset;
            for (int i = 0; i < useLen; ++i)
                dst[i] += weight[(size_t) i] * segCh[i];
        }
        for (int i = 0; i < useLen; ++i)
            sumW[(size_t) (offset + i)] += (double) weight[(size_t) i];

        ++segDone;
        if (progress)
            progress (juce::jlimit (0.0f, 1.0f, (float) segDone / (float) juce::jmax (1, numSeg)));
    }

    // Normalize each sample by the accumulated overlap weight.
    for (int ch = 0; ch < kModelChannels; ++ch)
    {
        float* d = out.getWritePointer (ch);
        for (int i = 0; i < L; ++i)
        {
            const double w = sumW[(size_t) i];
            if (w > 1e-8)
                d[i] = (float) ((double) d[i] / w);
        }
    }

    return out;
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

    struct Pass { const Model* model; int stemIdx; juce::AudioBuffer<float>* dst; };
    juce::AudioBuffer<float> drums44, bass44, vocals44;
    const Pass passes[] = {
        { drumsModel.get(),  Drums,  &drums44  },
        { bassModel.get(),   Bass,   &bass44   },
        { vocalsModel.get(), Vocals, &vocals44 },
    };
    constexpr int numModels = 3;

    for (int m = 0; m < numModels; ++m)
    {
        if (shouldAbort && shouldAbort())
            return result;

        auto mapped = [progress, m] (float f)
        {
            if (progress)
                progress (juce::jlimit (0.0f, 1.0f, ((float) m + f) / (float) numModels));
        };

        *passes[m].dst = runModel (*passes[m].model, mix44, passes[m].stemIdx, mapped, shouldAbort);
        if (passes[m].dst->getNumSamples() <= 0)
            return result; // aborted or failed → invalid result, caller keeps the original
    }

    // 2. Resample each stem back to the source rate and conform to source shape.
    result.drums  = conform (resample (drums44,  kModelSampleRate, sampleRate), srcCh, srcLen);
    result.bass   = conform (resample (bass44,   kModelSampleRate, sampleRate), srcCh, srcLen);
    result.vocals = conform (resample (vocals44, kModelSampleRate, sampleRate), srcCh, srcLen);
    result.valid  = true;

    if (progress) progress (1.0f);
    return result;
}
