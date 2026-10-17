// Offline regressions for the Beat This! frontend and beat decoder.
// No model is required for the default tests:
//   cmake -S . -B build -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF -DCUE_BUILD_TEMPO_TEST=ON
//   cmake --build build --target test_tempo_analysis
//   ./build/test_tempo_analysis_artefacts/Release/test_tempo_analysis
// Optional real-audio inspection (does not assert a song's musical tempo):
//   test_tempo_analysis --analyze song.wav [assets/beat_this.onnx]
//   test_tempo_analysis --features song.wav /tmp/cue-mel.txt
//   test_tempo_analysis --model-check [assets/beat_this.onnx]

#include "../BeatThisAnalyzer.h"
#include "../TempoAnalysisPolicy.h"
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Narrow friend seam: exercise the exact production frontend/decoder without
// requiring a neural model or making these implementation details public API.
struct BeatThisAnalyzerTests
{
    static auto decode (const std::vector<float>& beats,
                        const std::vector<float>& downbeats, double offset = 0.0)
    {
        return BeatThisAnalyzer::decodeBeatProbabilities (beats, downbeats, offset);
    }

    static auto features (const BeatThisAnalyzer& analyzer, const std::vector<float>& audio)
    {
        return analyzer.computeMelSpectrogram (audio);
    }

    static auto mono (const BeatThisAnalyzer& analyzer, const juce::AudioBuffer<float>& audio,
                      double rate, int start = 0, int end = -1)
    {
        return analyzer.resampleToMono (audio, rate, start, end < 0 ? audio.getNumSamples() : end);
    }

    static auto infer (const BeatThisAnalyzer& analyzer, int frames)
    {
        return analyzer.runOnnxInference (std::vector<float> ((size_t) frames * 128, 0.0f), frames);
    }
};

namespace
{
int failures = 0;
constexpr double fps = 50.0;
constexpr double targetRate = 22050.0;

void check (bool condition, const std::string& description)
{
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << description << '\n';
    if (! condition)
        ++failures;
}

struct Activations
{
    explicit Activations (double duration)
        : beat ((size_t) std::ceil (duration * fps), 0.01f), downbeat (beat.size(), 0.01f) {}

    void addBeat (double seconds, bool isDownbeat = false)
    {
        const int frame = (int) std::lround (seconds * fps);
        if (frame < 0 || frame >= (int) beat.size())
            return;

        beat[(size_t) frame] = 0.96f;
        expected.push_back ((double) frame / fps);
        if (isDownbeat)
        {
            downbeat[(size_t) frame] = 0.94f;
            expectedBars.push_back ((double) frame / fps);
        }
    }

    std::vector<float> beat, downbeat;
    std::vector<double> expected, expectedBars;
};

Activations regular (double bpm, int beatsPerBar = 4, bool swing = false)
{
    Activations input (25.0);
    const double period = 60.0 / bpm;
    for (int beat = 0; 0.4 + beat * period < 24.8; ++beat)
    {
        const double time = 0.4 + beat * period;
        input.addBeat (time, beat % beatsPerBar == 0);
        if (swing)
        {
            const size_t subdivision = (size_t) std::lround ((time + period * 2.0 / 3.0) * fps);
            if (subdivision < input.beat.size())
                input.beat[subdivision] = 0.18f; // uncertain swung subdivision, below beat threshold
        }
    }
    return input;
}

bool positionsMatch (const std::vector<double>& actual, const std::vector<double>& expected,
                     double offset = 0.0)
{
    if (actual.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (std::abs (actual[i] - expected[i] - offset) > 0.010001)
            return false;
    return true;
}

void decoderTests()
{
    std::cout << "Beat probability decoding\n";
    for (const double bpm : { 48.0, 60.0, 90.0, 120.0, 180.0, 230.0 })
    {
        const auto input = regular (bpm);
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat);
        const auto label = std::to_string ((int) bpm) + " BPM";
        check (output.valid && std::abs (output.estimatedBpm - bpm) / bpm < 0.025,
               label + " keeps its tempo level across the full supported range");
        check (positionsMatch (output.beatPositionsSecs, input.expected),
               label + " retains the predicted beat positions");
    }

    // Downbeat spacing describes bar grouping; it must not silently impose 4/4
    // when recovering the tempo from beat activations.
    for (const auto& [grouping, name] : std::array<std::pair<int, const char*>, 3>
         {{ { 3, "3/4" }, { 2, "6/8 (dotted-quarter pulse)" }, { 7, "7/8 (eighth-note pulse)" } }})
    {
        const auto input = regular (120.0, grouping);
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat);
        check (output.valid && std::abs (output.estimatedBpm - 120.0) < 0.5,
               std::string (name) + " downbeats do not rescale beat tempo");
        check (positionsMatch (output.barPositionsSecs, input.expectedBars),
               std::string (name) + " retains predicted downbeats");
    }

    {
        const auto input = regular (96.0, 4, true);
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat, 12.34);
        check (output.valid && std::abs (output.estimatedBpm - 96.0) < 2.0
                   && positionsMatch (output.beatPositionsSecs, input.expected, 12.34),
               "swing subdivisions do not replace the steady pulse; crop offset is preserved");
        check (positionsMatch (output.barPositionsSecs, input.expectedBars, 12.34)
                   && std::abs (output.firstBeatSecs - input.expected.front() - 12.34) < 0.011,
               "crop offset is applied equally to first beat and downbeats");
    }

    {
        Activations input (25.0);
        double seconds = 0.4;
        for (int beat = 0; seconds < 24.5; ++beat)
        {
            input.addBeat (seconds, beat % 4 == 0);
            seconds += beat < 20 ? 0.75 : 0.45;
        }
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat);
        check (output.valid && output.likelyDrifting,
               "a material local tempo change is reported");
        check (positionsMatch (output.beatPositionsSecs, input.expected),
               "changing tempo keeps actual beat times rather than forcing one global grid");
    }

    {
        Activations input (25.0);
        for (int beat = 0; beat < 45; ++beat)
            if (beat % 7 != 3)
                input.addBeat (0.4 + beat * 0.5, beat % 4 == 0);
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat);
        check (output.valid && positionsMatch (output.beatPositionsSecs, input.expected),
               "missing beat activations do not cause fabricated beat markers");
    }

    {
        std::vector<float> silence (1000, 0.0f);
        check (! BeatThisAnalyzerTests::decode (silence, silence).valid,
               "silence has no valid tempo");
        Activations brief (3.0);
        brief.addBeat (0.5);
        brief.addBeat (1.0);
        brief.addBeat (1.5);
        check (! BeatThisAnalyzerTests::decode (brief.beat, brief.downbeat).valid,
               "three isolated beat predictions are insufficient for a reliable tempo");
        check (! BeatThisAnalyzerTests::decode ({}, {}).valid,
               "empty predictions return an invalid result");
        silence[20] = std::numeric_limits<float>::quiet_NaN();
        silence[100] = std::numeric_limits<float>::infinity();
        silence[200] = -std::numeric_limits<float>::infinity();
        check (! BeatThisAnalyzerTests::decode (silence, silence).valid,
               "non-finite predictions cannot create a valid tempo");
    }

    {
        Activations input (25.0);
        for (int beat = 0; beat < 45; ++beat)
        {
            input.addBeat (0.4 + beat * 0.5);
            input.addBeat (0.48 + beat * 0.5); // strong nearby echo/flam, not another steady beat
        }
        const auto output = BeatThisAnalyzerTests::decode (input.beat, input.downbeat);
        check (! output.valid || output.confidence < 0.55f,
               "dense false peak pairs cannot earn strong tempo confidence by discarding short intervals");
    }
}

void selectionPolicyTests()
{
    struct Analysis
    {
        double estimatedBpm = 120.0;
        double beatPeriodSeconds = 0.5;
        float confidence = 0.9f;
        bool likelyDrifting = false;
        std::vector<double> beatPositionsSeconds;
    };

    std::cout << "\nClassical/neural selection policy\n";
    check (cuesampler::temposAgreeAtDifferentOctaves (107.916, 54.005)
               && cuesampler::temposAgreeAtDifferentOctaves (54.005, 107.916),
           "the observed 107.916/54.005 BPM regression is recognized as metrical ambiguity");
    check (cuesampler::temposAgreeAtDifferentOctaves (180.0, 90.0),
           "180/90 BPM is recognized as an octave disagreement");
    check (! cuesampler::temposAgreeAtDifferentOctaves (98.0, 100.0)
               && ! cuesampler::temposAgreeAtDifferentOctaves (139.0, 59.0),
           "nearby BPMs and unrelated tempo estimates are not octave disagreements");
    check (! cuesampler::temposAgreeAtDifferentOctaves (std::numeric_limits<double>::infinity(), 90.0)
               && ! cuesampler::temposAgreeAtDifferentOctaves (90.0, std::numeric_limits<double>::quiet_NaN())
               && ! cuesampler::temposAgreeAtDifferentOctaves (0.0, 90.0),
           "invalid BPMs cannot trigger metrical-level preservation");

    Analysis steady;
    for (int beat = 0; beat <= 32; ++beat)
        steady.beatPositionsSeconds.push_back (beat * 0.5);
    check (cuesampler::canUseClassicalTempoWithoutNeuralCheck (steady),
           "a confident, regular, sufficiently long classical grid retains the fast path");

    auto moderate = steady;
    moderate.confidence = 0.6f;
    check (! cuesampler::canUseClassicalTempoWithoutNeuralCheck (moderate),
           "moderate classical confidence requests neural corroboration");
    auto jittered = steady;
    jittered.beatPositionsSeconds[8] += 0.03;
    auto missingBeat = steady;
    missingBeat.beatPositionsSeconds.erase (missingBeat.beatPositionsSeconds.begin() + 8);
    check (! cuesampler::canUseClassicalTempoWithoutNeuralCheck (jittered)
               && ! cuesampler::canUseClassicalTempoWithoutNeuralCheck (missingBeat),
           "jitter and missing beats request neural corroboration");
    auto drifting = steady;
    drifting.likelyDrifting = true;
    auto shortClip = steady;
    shortClip.beatPositionsSeconds.resize (8);
    check (! cuesampler::canUseClassicalTempoWithoutNeuralCheck (drifting)
               && ! cuesampler::canUseClassicalTempoWithoutNeuralCheck (shortClip),
           "drift and short ambiguous clips request neural corroboration");

    auto doubleTime = steady;
    doubleTime.estimatedBpm = 240.0;
    doubleTime.beatPeriodSeconds = 0.25;
    doubleTime.beatPositionsSeconds.clear();
    for (int beat = 0; beat <= 64; ++beat)
        doubleTime.beatPositionsSeconds.push_back (beat * 0.25);
    const auto score = cuesampler::tempoAnalysisEvidenceScore (steady);
    check (score == cuesampler::tempoAnalysisEvidenceScore (doubleTime)
               && score == cuesampler::tempoAnalysisEvidenceScore (drifting),
           "evidence scoring does not favor double-time or reject drift by itself");

    auto invalid = steady;
    invalid.confidence = std::numeric_limits<float>::quiet_NaN();
    check (! cuesampler::canUseClassicalTempoWithoutNeuralCheck (invalid)
               && cuesampler::tempoAnalysisEvidenceScore (invalid) == 0.0,
           "non-finite confidence cannot bypass validation or earn an evidence score");
    invalid = steady;
    invalid.beatPositionsSeconds.back() = std::numeric_limits<double>::infinity();
    check (! cuesampler::canUseClassicalTempoWithoutNeuralCheck (invalid)
               && cuesampler::tempoAnalysisEvidenceScore (invalid) == 0.0,
           "non-finite grid timing cannot earn an evidence score");
}

std::vector<float> referenceWaveform()
{
    std::vector<float> result (2205);
    for (size_t i = 0; i < result.size(); ++i)
    {
        const double phase = juce::MathConstants<double>::twoPi * (double) i / targetRate;
        result[i] = (float) (0.19 * std::sin (37.0 * phase) + 0.31 * std::sin (440.0 * phase)
                             + 0.13 * std::cos (3990.0 * phase) + 0.07 * std::sin (9300.0 * phase)
                             + (i == 512 ? 0.43 : 0.0) + (i == 2203 ? 0.27 : 0.0));
    }
    return result;
}

double rmsTrimmed (const std::vector<float>& samples)
{
    constexpr size_t trim = 400;
    if (samples.size() <= 2 * trim)
        return 0.0;
    double sum = 0.0;
    for (size_t i = trim; i < samples.size() - trim; ++i)
        sum += (double) samples[i] * samples[i];
    return std::sqrt (sum / (double) (samples.size() - 2 * trim));
}

void frontendTests (const BeatThisAnalyzer& analyzer)
{
    std::cout << "\nModel frontend\n";
    // Independent golden values generated with PyTorch/torchaudio 2.4.1:
    // MelSpectrogram(sample_rate=22050,n_fft=1024,hop_length=441,f_min=30,
    // f_max=11000,n_mels=128,mel_scale='slaney',normalized='frame_length',
    // power=1,center=True,pad_mode='reflect',norm=None), then log1p(1000*mel.T).
    // The waveform includes low/high tones plus interior/boundary impulses.
    // These values catch mel spacing, scaling, periodic Hann, and reflection.
    constexpr std::array<int, 16> bands { 0, 3, 7, 13, 20, 31, 40, 55, 70, 90, 100, 110, 116, 120, 124, 127 };
    constexpr float expected[6][16] {
        { 6.027684689f, 4.414970875f, 5.431170940f, 6.987477779f, 5.644056320f, 4.235373020f, 3.704384089f, 3.258070469f, 2.798070192f, 6.979687691f, 1.072074652f, 2.319704056f, 3.649482965f, 5.323845863f, 5.293829918f, 4.853410244f },
        { 6.933977127f, 3.009760380f, 2.590344667f, 5.077371597f, 2.977527857f, 2.808716297f, 2.814474583f, 3.262792587f, 3.645898819f, 6.991982937f, 4.421212196f, 4.687505245f, 4.841412067f, 4.946190834f, 5.050344467f, 5.129019260f },
        { 6.938361168f, 1.793546796f, 1.329320669f, 4.995620251f, 2.102782726f, 1.338519931f, 1.386244535f, 1.739339113f, 2.074475288f, 6.983153820f, 2.794544220f, 3.049125671f, 3.197517633f, 3.298194170f, 3.400114298f, 3.476857901f },
        { 6.941588402f, 1.783928275f, 0.813566864f, 5.005335331f, 1.722557306f, 0.126234204f, 0.037420813f, 0.010696634f, 0.004658610f, 6.981804848f, 0.008077883f, 0.001046394f, 0.002928583f, 0.162928119f, 0.039659973f, 0.001975904f },
        { 6.937616825f, 2.286738634f, 2.580488443f, 5.072719097f, 2.758021355f, 1.548791528f, 1.234099984f, 1.094434142f, 0.865188539f, 6.985321045f, 1.614545226f, 0.286204338f, 0.918510914f, 1.631734133f, 2.803793430f, 2.689783096f },
        { 7.187507629f, 5.421965122f, 5.557856560f, 6.983939648f, 5.657436848f, 4.362020493f, 3.947198391f, 3.744560242f, 3.384252548f, 7.427525997f, 4.439103127f, 1.831934452f, 3.471517086f, 4.286336899f, 5.785385132f, 5.659908772f }
    };
    const auto actual = BeatThisAnalyzerTests::features (analyzer, referenceWaveform());
    check (actual.size() == 6 * 128, "centered STFT includes the last frame at an exact hop boundary");
    double maxError = 0.0;
    if (actual.size() == 6 * 128)
        for (size_t frame = 0; frame < 6; ++frame)
            for (size_t band = 0; band < bands.size(); ++band)
                maxError = std::max (maxError, std::abs ((double) actual[frame * 128 + (size_t) bands[band]]
                                                      - expected[frame][band]));
    else
        maxError = std::numeric_limits<double>::infinity();
    check (maxError < 0.003, "mel features match independent torchaudio reference (max error "
                             + std::to_string (maxError) + ")");

    const auto silence = BeatThisAnalyzerTests::features (analyzer, std::vector<float> (2205, 0.0f));
    check (silence.size() == 6 * 128
               && std::all_of (silence.begin(), silence.end(), [] (float v) { return v == 0.0f; }),
           "silent features are finite zeros");
    const auto shortInput = BeatThisAnalyzerTests::features (analyzer, { 0.25f });
    check (shortInput.size() == 128
               && std::all_of (shortInput.begin(), shortInput.end(), [] (float v) { return std::isfinite (v); }),
           "single-sample input has finite reflected features");

    juce::AudioBuffer<float> stereo (2, 1024);
    for (int i = 0; i < stereo.getNumSamples(); ++i)
    {
        stereo.setSample (0, i, (float) i / 1024.0f);
        stereo.setSample (1, i, -(float) i / 2048.0f);
    }
    const auto mono = BeatThisAnalyzerTests::mono (analyzer, stereo, targetRate, 100, 900);
    bool exact = mono.size() == 800;
    for (size_t i = 0; exact && i < mono.size(); ++i)
        exact = mono[i] == (stereo.getSample (0, (int) i + 100) + stereo.getSample (1, (int) i + 100)) * 0.5f;
    check (exact, "same-rate cropped stereo downmix retains exact samples");

    auto resampleTone = [&analyzer] (double frequency)
    {
        juce::AudioBuffer<float> source (1, 48000);
        for (int i = 0; i < source.getNumSamples(); ++i)
            source.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / 48000.0));
        return BeatThisAnalyzerTests::mono (analyzer, source, 48000.0);
    };
    const auto lowTone = resampleTone (1000.0);
    const auto highTone = resampleTone (16000.0);
    const double lowRms = rmsTrimmed (lowTone), highRms = rmsTrimmed (highTone);
    check (lowTone.size() == 22050 && std::abs (lowRms - std::sqrt (0.5)) < 0.015,
           "48 kHz resampling preserves a 1 kHz passband tone and target length");
    check (highTone.size() == 22050 && highRms < 0.00224,
           "48 kHz resampling suppresses a 16 kHz alias by at least 50 dB (RMS "
               + std::to_string (highRms) + ")");
}

struct LoadedAudio
{
    juce::AudioBuffer<float> buffer;
    double rate = 0.0;
};

bool loadAudio (const juce::String& path, LoadedAudio& result)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (path)));
    if (reader == nullptr || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        std::cerr << "Cannot read audio: " << path << '\n';
        return false;
    }
    result.rate = reader->sampleRate;
    result.buffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    return reader->read (&result.buffer, 0, result.buffer.getNumSamples(), 0, true, true);
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc >= 2 && std::string (argv[1]) == "--model-check")
    {
        BeatThisAnalyzer model (argc >= 3 ? juce::String::fromUTF8 (argv[2]) : juce::String ("assets/beat_this.onnx"));
        if (! model.isReady())
        {
            std::cerr << "Model could not be loaded\n";
            return 2;
        }
        for (const int frames : { 1490, 1501, 3000 })
        {
            const auto output = BeatThisAnalyzerTests::infer (model, frames);
            check (output.beat.size() == (size_t) frames && output.downbeat.size() == (size_t) frames,
                   std::to_string (frames) + " frames retain exact beat/downbeat lengths across chunk boundaries");
            const auto finite = [] (float value) { return std::isfinite (value); };
            check (! output.beat.empty() && ! output.downbeat.empty()
                       && std::all_of (output.beat.begin(), output.beat.end(), finite)
                       && std::all_of (output.downbeat.begin(), output.downbeat.end(), finite),
                   std::to_string (frames) + " frames produce finite model probabilities");
        }
        return failures == 0 ? 0 : 1;
    }

    if (argc >= 3 && std::string (argv[1]) == "--analyze")
    {
        LoadedAudio audio;
        if (! loadAudio (juce::String::fromUTF8 (argv[2]), audio))
            return 2;
        BeatThisAnalyzer analyzer (argc >= 4 ? juce::String::fromUTF8 (argv[3]) : juce::String ("assets/beat_this.onnx"));
        if (! analyzer.isReady())
        {
            std::cerr << "Model could not be loaded\n";
            return 2;
        }
        const auto start = std::chrono::steady_clock::now();
        const auto result = analyzer.analyze (audio.buffer, audio.rate);
        const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::cout << std::setprecision (9) << "valid=" << result.valid << " bpm=" << result.estimatedBpm
                  << " confidence=" << result.confidence << " drifting=" << result.likelyDrifting
                  << " analysis_seconds=" << elapsed << '\n';
        for (const auto seconds : result.beatPositionsSecs)
            std::cout << "beat," << seconds << '\n';
        for (const auto seconds : result.barPositionsSecs)
            std::cout << "downbeat," << seconds << '\n';
        return result.valid ? 0 : 1;
    }

    BeatThisAnalyzer analyzer ("/nonexistent/cue-tempo-test/no-model.onnx");
    if (argc == 4 && std::string (argv[1]) == "--features")
    {
        LoadedAudio audio;
        if (! loadAudio (juce::String::fromUTF8 (argv[2]), audio))
            return 2;
        const auto mono = BeatThisAnalyzerTests::mono (analyzer, audio.buffer, audio.rate);
        const auto features = BeatThisAnalyzerTests::features (analyzer, mono);
        std::ofstream file (argv[3]);
        file << std::setprecision (9);
        for (size_t i = 0; i < features.size(); ++i)
            file << features[i] << ((i + 1) % 128 == 0 ? '\n' : ' ');
        return file.good() ? 0 : 2;
    }
    if (argc != 1)
    {
        std::cerr << "Usage: test_tempo_analysis [--analyze audio [model] | --features audio output.txt | --model-check [model]]\n";
        return 2;
    }

    decoderTests();
    selectionPolicyTests();
    frontendTests (analyzer);
    std::cout << '\n' << failures << " failed check(s)\n";
    return failures == 0 ? 0 : 1;
}
