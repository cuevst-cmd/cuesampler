#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace cuesampler
{
// Display-only analysis. Run on a worker, never in processBlock. Stereo channel
// energies are summed independently so out-of-phase audio cannot disappear.
struct WaveformColourAnalysis
{
    int hop = 1;
    int sampleCount = 0;
    std::vector<std::array<double, 3>> energyPrefix;
    std::vector<int> attacks;

    std::array<double, 3> bands (int begin, int end) const noexcept
    {
        if (energyPrefix.size() < 2) return {};
        const int frames = (int) energyPrefix.size() - 1;
        const int first = juce::jlimit (0, frames - 1, begin / hop);
        const int last = juce::jlimit (first + 1, frames, (end + hop - 1) / hop);
        std::array<double, 3> result {};
        for (size_t band = 0; band < result.size(); ++band)
            result[band] = (energyPrefix[(size_t) last][band] - energyPrefix[(size_t) first][band]) / (last - first);
        return result;
    }

    bool hasAttack (int begin, int end) const noexcept
    {
        const auto it = std::lower_bound (attacks.begin(), attacks.end(), begin);
        return it != attacks.end() && *it < end;
    }

    static std::shared_ptr<const WaveformColourAnalysis> analyse (
        const juce::AudioBuffer<float>& audio, double sampleRate,
        const std::function<bool()>& cancelled = [] { return false; })
    {
        auto result = std::make_shared<WaveformColourAnalysis>();
        if (! std::isfinite (sampleRate) || sampleRate <= 0 || audio.getNumChannels() == 0)
            return result;
        result->sampleCount = audio.getNumSamples();
        result->hop = juce::jmax (1, (int) std::round (sampleRate * 0.005));
        result->energyPrefix.reserve ((size_t) (result->sampleCount / result->hop + 2));
        result->energyPrefix.push_back ({});
        std::vector<std::array<double, 3>> filters ((size_t) audio.getNumChannels());
        const double lowT = std::tan (juce::MathConstants<double>::pi * juce::jmin (250.0, sampleRate * 0.1) / sampleRate);
        const double highT = std::tan (juce::MathConstants<double>::pi * juce::jmin (4000.0, sampleRate * 0.4) / sampleRate);
        const double lowB = lowT / (1.0 + lowT), lowA = (1.0 - lowT) / (1.0 + lowT);
        const double highB = highT / (1.0 + highT), highA = (1.0 - highT) / (1.0 + highT);
        const double decay = std::exp (-(double) result->hop / (sampleRate * 0.080));
        double background = 0.0, previousEnergy = 0.0;
        int lastAttack = -(int) sampleRate;
        for (int begin = 0; begin < result->sampleCount; begin += result->hop)
        {
            if (cancelled()) return {};
            const int end = juce::jmin (begin + result->hop, result->sampleCount);
            std::array<double, 3> energy {};
            double fullEnergy = 0.0, peak = 0.0;
            int peakSample = begin;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            {
                auto& filter = filters[(size_t) ch];
                const auto* samples = audio.getReadPointer (ch);
                for (int i = begin; i < end; ++i)
                {
                    const double x = std::isfinite (samples[i]) ? (double) samples[i] : 0.0;
                    filter[0] = lowB * (x + filter[2]) + lowA * filter[0];
                    filter[1] = highB * (x + filter[2]) + highA * filter[1];
                    filter[2] = x;
                    const std::array<double, 3> values { filter[0], filter[1] - filter[0], x - filter[1] };
                    for (size_t band = 0; band < values.size(); ++band)
                        energy[band] += values[band] * values[band];
                    fullEnergy += x * x;
                    if (std::abs (x) > peak) { peak = std::abs (x); peakSample = i; }
                }
            }
            const double count = (double) (end - begin) * audio.getNumChannels();
            auto prefix = result->energyPrefix.back();
            for (size_t band = 0; band < energy.size(); ++band) prefix[band] += energy[band] / count;
            result->energyPrefix.push_back (prefix);
            const double rmsEnergy = fullEnergy / count;
            // A short energy rise against an 80ms background, with a noise floor
            // and 30ms spacing. These are visual attack hints, not slice edits.
            if (peak > 0.003 && rmsEnergy > juce::jmax (1.0e-8, background * 3.0)
                && rmsEnergy > previousEnergy * 1.8
                && peakSample - lastAttack >= (int) (sampleRate * 0.030))
            {
                result->attacks.push_back (peakSample);
                lastAttack = peakSample;
            }
            background = decay * background + (1.0 - decay) * rmsEnergy;
            previousEnergy = rmsEnergy;
        }
        return result;
    }
};
}
