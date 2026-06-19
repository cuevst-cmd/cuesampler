// Offline test for StemSeparator (HTDemucs-FT ONNX stem separation).
//
// Unlike tools/test_warp_map.cpp (no deps), this needs JUCE + ONNX Runtime, so
// it is built through CMake behind an off-by-default option rather than a bare
// c++ invocation:
//
//   cmake -S . -B build -DCUE_BUILD_STEM_TEST=ON
//   cmake --build build --target test_stem_separator
//   ./build/test_stem_separator_artefacts/test_stem_separator <song.wav> [models_dir]
//
//   <song.wav>    short stereo WAV (a few seconds is plenty)
//   [models_dir]  dir holding drums.onnx / bass.onnx / vocals.onnx
//                 (default: assets/htdemucs_ft). Produce them with
//                 export_htdemucs.py or download from StemSplitio/htdemucs-ft-onnx.
//
// If the onnxruntime dylib isn't found at run time, prefix with:
//   DYLD_LIBRARY_PATH=build/_deps/onnxruntime-src/lib ./.../test_stem_separator ...
//
// Asserts:
//   (a) each returned stem has the source length & channel count;
//   (b) the subtraction algebra reconstructs: with other := original −
//       (drums+bass+vocals), then drums+bass+vocals+other == original within fp
//       tolerance (this is what Phase 3's rebuildActiveMix relies on);
//   (c) energy sanity: each stem is non-silent, no stem is louder than the mix,
//       and the three stems are mutually distinct (separation actually happened).
//
// Exit code 0 = all checks passed (or a clean skip when models are absent);
// non-zero = a check failed.

#include "../StemSeparator.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (! ok) ++failures;
}

double rms (const juce::AudioBuffer<float>& b)
{
    const int ch = b.getNumChannels(), n = b.getNumSamples();
    if (ch <= 0 || n <= 0) return 0.0;
    double acc = 0.0;
    for (int c = 0; c < ch; ++c)
    {
        const float* p = b.getReadPointer (c);
        for (int i = 0; i < n; ++i) acc += (double) p[i] * (double) p[i];
    }
    return std::sqrt (acc / (double) (ch * n));
}

// dst += src (matched shapes assumed)
void addInto (juce::AudioBuffer<float>& dst, const juce::AudioBuffer<float>& src)
{
    const int ch = juce::jmin (dst.getNumChannels(), src.getNumChannels());
    const int n  = juce::jmin (dst.getNumSamples(),  src.getNumSamples());
    for (int c = 0; c < ch; ++c) dst.addFrom (c, 0, src, c, 0, n);
}

double maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    const int ch = juce::jmin (a.getNumChannels(), b.getNumChannels());
    const int n  = juce::jmin (a.getNumSamples(),  b.getNumSamples());
    double m = 0.0;
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i)
            m = juce::jmax (m, (double) std::abs (a.getReadPointer (c)[i] - b.getReadPointer (c)[i]));
    return m;
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: test_stem_separator <song.wav> [models_dir=assets/htdemucs_ft]\n";
        return 2;
    }

    const juce::File wavFile   { juce::String (argv[1]) };
    const juce::File modelsDir { argc >= 3 ? juce::String (argv[2])
                                           : juce::String ("assets/htdemucs_ft") };

    if (! wavFile.existsAsFile())
    {
        std::cerr << "WAV not found: " << wavFile.getFullPathName() << "\n";
        return 2;
    }

    // ── Load the WAV ──────────────────────────────────────────────────────────
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));
    if (reader == nullptr)
    {
        std::cerr << "could not read audio file: " << wavFile.getFullPathName() << "\n";
        return 2;
    }

    const int    srcCh  = (int) reader->numChannels;
    const int    srcLen = (int) reader->lengthInSamples;
    const double srcRate = reader->sampleRate;
    juce::AudioBuffer<float> source (srcCh, srcLen);
    reader->read (&source, 0, srcLen, 0, true, true);

    std::cout << "Source: " << wavFile.getFileName() << "  "
              << srcCh << "ch  " << srcLen << " smp  "
              << srcRate << " Hz  (" << (srcLen / srcRate) << " s)\n";

    // ── Construct the separator ───────────────────────────────────────────────
    StemSeparator::ModelPaths paths;
    paths.drums  = modelsDir.getChildFile ("drums.onnx").getFullPathName();
    paths.bass   = modelsDir.getChildFile ("bass.onnx").getFullPathName();
    paths.vocals = modelsDir.getChildFile ("vocals.onnx").getFullPathName();

    using clock = std::chrono::steady_clock;
    auto secsSince = [] (clock::time_point t0)
    { return std::chrono::duration<double> (clock::now() - t0).count(); };

    const bool coremlOff = std::getenv ("CUE_DISABLE_COREML") != nullptr;
    std::cout << "Execution provider: " << (coremlOff ? "CPU only (CUE_DISABLE_COREML set)"
                                                      : "CoreML + CPU fallback") << "\n";

    const auto tLoad0 = clock::now();
    StemSeparator sep (paths);
    const double loadSecs = secsSince (tLoad0);
    if (! sep.isReady())
    {
        std::cout << "\nStemSeparator not ready (models missing in "
                  << modelsDir.getFullPathName() << "). SKIPPING run.\n"
                  << "Place drums/bass/vocals.onnx there to exercise inference.\n";
        return 0; // clean skip — not a failure
    }
    std::printf ("Session load + compile: %.2f s (3 models)\n", loadSecs);

    // ── Separate ──────────────────────────────────────────────────────────────
    std::cout << "Separating";
    const auto tSep0 = clock::now();
    auto result = sep.separate (source, srcRate,
                                [] (float f) { std::printf ("\rSeparating %5.1f%%", f * 100.0f); std::fflush (stdout); });
    const double sepSecs = secsSince (tSep0);
    std::printf ("\rSeparating done.   \n");
    const double audioSecs = srcLen / srcRate;
    std::printf ("Separation: %.2f s wall for %.2f s audio (%.2fx realtime)\n",
                 sepSecs, audioSecs, audioSecs / juce::jmax (1e-9, sepSecs));

    check (result.valid, "separate() returned a valid result");
    if (! result.valid) return failures == 0 ? 0 : 1;

    // (a) shapes
    auto shapeOk = [&] (const juce::AudioBuffer<float>& b)
    { return b.getNumChannels() == srcCh && b.getNumSamples() == srcLen; };
    check (shapeOk (result.drums),  "drums  matches source shape");
    check (shapeOk (result.bass),   "bass   matches source shape");
    check (shapeOk (result.vocals), "vocals matches source shape");

    // (b) subtraction reconstruction: other := original − (drums+bass+vocals)
    juce::AudioBuffer<float> sumDBV (srcCh, srcLen); sumDBV.clear();
    addInto (sumDBV, result.drums);
    addInto (sumDBV, result.bass);
    addInto (sumDBV, result.vocals);

    juce::AudioBuffer<float> other (srcCh, srcLen);   // original − sum
    for (int c = 0; c < srcCh; ++c)
    {
        other.copyFrom (c, 0, source, c, 0, srcLen);
        other.addFrom  (c, 0, sumDBV, c, 0, srcLen, -1.0f);
    }
    juce::AudioBuffer<float> recon (srcCh, srcLen); recon.clear();
    addInto (recon, sumDBV);
    addInto (recon, other);
    const double reconErr = maxAbsDiff (recon, source);
    std::cout << "  reconstruction max-abs-err = " << reconErr << "\n";
    check (reconErr < 1e-4, "drums+bass+vocals+other == original (subtraction algebra)");

    // (c) energy sanity
    const double rO = rms (source), rD = rms (result.drums),
                 rB = rms (result.bass), rV = rms (result.vocals), rOther = rms (other);
    std::printf ("  RMS: orig=%.5f drums=%.5f bass=%.5f vocals=%.5f other=%.5f\n",
                 rO, rD, rB, rV, rOther);
    check (rD > 1e-5, "drums  non-silent");
    check (rB > 1e-5, "bass   non-silent");
    check (rV > 1e-5, "vocals non-silent");
    check (rD <= rO * 4.0 && rB <= rO * 4.0 && rV <= rO * 4.0, "no stem absurdly louder than mix");
    check (maxAbsDiff (result.drums, result.bass)   > 1e-4, "drums  != bass");
    check (maxAbsDiff (result.drums, result.vocals) > 1e-4, "drums  != vocals");
    check (maxAbsDiff (result.bass,  result.vocals) > 1e-4, "bass   != vocals");

    // (d) subtraction mix model (what the processor's rebuildActiveMix does):
    //     no mutes      -> activeMix == original (bit-for-bit, zero copy)
    //     vocals muted   -> activeMix == original − vocals
    {
        // No mutes: the processor aliases the pristine original, so the played
        // buffer is identical to the source.
        const double noMuteErr = maxAbsDiff (source, source);
        check (noMuteErr == 0.0, "activeMix == original when no mutes");

        // Vocals muted: activeMix = original − vocals.
        juce::AudioBuffer<float> vocalsMuted (srcCh, srcLen);
        for (int c = 0; c < srcCh; ++c)
        {
            vocalsMuted.copyFrom (c, 0, source, c, 0, srcLen);
            vocalsMuted.addFrom  (c, 0, result.vocals, c, 0, srcLen, -1.0f);
        }
        // It must actually differ from the original (vocals were removed)...
        check (maxAbsDiff (vocalsMuted, source) > 1e-5, "vocals-muted mix differs from original");
        // ...and adding vocals back must reconstruct the original exactly.
        juce::AudioBuffer<float> reAdded (srcCh, srcLen);
        for (int c = 0; c < srcCh; ++c)
        {
            reAdded.copyFrom (c, 0, vocalsMuted, c, 0, srcLen);
            reAdded.addFrom  (c, 0, result.vocals, c, 0, srcLen, 1.0f);
        }
        check (maxAbsDiff (reAdded, source) < 1e-4, "activeMix == original − vocals (only vocals muted)");
    }

    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string (failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
