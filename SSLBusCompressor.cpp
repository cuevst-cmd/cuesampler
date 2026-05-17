#include "SSLBusCompressor.h"

#include <algorithm>
#include <cmath>

namespace cuesampler
{

namespace
{
    constexpr float kEpsilon        = 1.0e-6f;
    constexpr float kAttackSec      = 0.001f;   // Fast SSL-style grab without lookahead
    constexpr float kSlowAttackSec  = 0.300f;   // slow path lags so it only engages on sustained GR
    constexpr float kFastReleaseSec = 0.100f;   // ~100 ms
    constexpr float kSlowReleaseSec = 1.500f;   // ~1.5 s
    constexpr float kKneeWidthDb    = 10.0f;
    constexpr float kSidechainHz    = 20.0f;    // DC/rumble cleanup only; classic SSL sidechain is full-band
    constexpr float kMaxGrDb        = 30.0f;
    constexpr float kRatioSlope     = 0.75f;    // ratio locked at 4:1  →  1 - 1/4
    constexpr float kAnalogCalibrationDb = 18.0f;

    inline float onepoleCoeff (float timeSec, float fs) noexcept
    {
        if (timeSec <= 0.0f || fs <= 0.0f) return 0.0f;
        return std::exp (-1.0f / (timeSec * fs));
    }

    inline float dbToLin (float dB) noexcept
    {
        return std::pow (10.0f, dB * (1.0f / 20.0f));
    }

    inline float linToDb (float lin) noexcept
    {
        return 20.0f * std::log10 (std::max (lin, kEpsilon));
    }
}

void SSLBusCompressor::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    juce::ignoreUnused (numChannels);
    fs = sampleRate;

    const juce::dsp::ProcessSpec spec { sampleRate,
                                        (juce::uint32) juce::jmax (1, maxBlockSize),
                                        1 };

    auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, kSidechainHz, 0.707f);
    scHpfL.coefficients = coeffs;
    scHpfR.coefficients = coeffs;
    scHpfL.prepare (spec);
    scHpfR.prepare (spec);

    smoothedThreshold.reset (sampleRate, 0.020);
    smoothedMakeupLin.reset (sampleRate, 0.020);

    updateCoefficients();
    reset();
}

void SSLBusCompressor::reset()
{
    grFastDb = grSlowDb = 0.0f;
    scHpfL.reset();
    scHpfR.reset();
    currentGrDb.store (0.0f, std::memory_order_release);
}

void SSLBusCompressor::setParametersImmediately (bool shouldEnable, float thresholdDbIn, float makeupDbIn) noexcept
{
    const auto clampedThreshold = juce::jlimit (-15.0f, 15.0f, thresholdDbIn);
    const auto clampedMakeup = juce::jlimit (0.0f, 20.0f, makeupDbIn);

    enabled.store (shouldEnable, std::memory_order_release);
    thresholdDb.store (clampedThreshold, std::memory_order_release);
    makeupDb.store (clampedMakeup, std::memory_order_release);
    smoothedThreshold.setCurrentAndTargetValue (clampedThreshold);
    smoothedMakeupLin.setCurrentAndTargetValue (dbToLin (clampedMakeup));
}

void SSLBusCompressor::updateCoefficients()
{
    const auto sr = (float) fs;
    attackCoeff     = onepoleCoeff (kAttackSec,      sr);
    attackSlowCoeff = onepoleCoeff (kSlowAttackSec,  sr);
    relFastCoeff    = onepoleCoeff (kFastReleaseSec, sr);
    relSlowCoeff    = onepoleCoeff (kSlowReleaseSec, sr);
}

void SSLBusCompressor::process (juce::AudioBuffer<float>& buffer)
{
    if (! enabled.load (std::memory_order_acquire))
    {
        currentGrDb.store (0.0f, std::memory_order_release);
        return;
    }

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jlimit (1, 2, buffer.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0) return;

    smoothedThreshold.setTargetValue (thresholdDb.load (std::memory_order_acquire));
    smoothedMakeupLin.setTargetValue (dbToLin (makeupDb.load (std::memory_order_acquire)));

    auto* L = buffer.getWritePointer (0);
    auto* R = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

    float maxGrDb = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        const float thr = smoothedThreshold.getNextValue();
        const float mu  = smoothedMakeupLin.getNextValue();

        const float xL = L[n];
        const float xR = R ? R[n] : xL;

        // Feedforward sidechain: HPF the signed input, rectify, stereo-link
        // by taking the loudest channel.
        const float scL = scHpfL.processSample (xL);
        const float scR = R ? scHpfR.processSample (xR) : scL;
        const float scLevel = juce::jmax (std::abs (scL), std::abs (scR));

        // SSL threshold markings are not dBFS. Treat -18 dBFS as the analog
        // operating reference so threshold values behave like a calibrated bus
        // compressor rather than a digital peak compressor.
        const float levelDb = linToDb (scLevel) + kAnalogCalibrationDb;
        const float over    = levelDb - thr;

        // Soft-knee static curve (quadratic blend across knee width).
        float grStaticDb;
        if (over <= -kKneeWidthDb * 0.5f)
            grStaticDb = 0.0f;
        else if (over >= kKneeWidthDb * 0.5f)
            grStaticDb = kRatioSlope * over;
        else
        {
            const float u = over + kKneeWidthDb * 0.5f;
            grStaticDb = kRatioSlope * (u * u) / (2.0f * kKneeWidthDb);
        }

        // Dual-envelope auto release. Both run in parallel; the louder
        // (more-reducing) one wins. Slow path has a slow attack so it only
        // builds up under sustained gain reduction.
        const float aFast = (grStaticDb > grFastDb) ? attackCoeff : relFastCoeff;
        grFastDb = aFast * grFastDb + (1.0f - aFast) * grStaticDb;

        const float aSlow = (grStaticDb > grSlowDb) ? attackSlowCoeff : relSlowCoeff;
        grSlowDb = aSlow * grSlowDb + (1.0f - aSlow) * grStaticDb;

        float grEnvDb = juce::jmax (grFastDb, grSlowDb);
        grEnvDb = juce::jlimit (0.0f, kMaxGrDb, grEnvDb);

        const float grLin = dbToLin (-grEnvDb);

        // VCA + manual makeup only. Do not limit, clip, or saturate here;
        // output level remains the user's gain-staging responsibility.
        const float gain = grLin * mu;
        L[n] = xL * gain;
        if (R) R[n] = xR * gain;

        if (grEnvDb > maxGrDb) maxGrDb = grEnvDb;
    }

    currentGrDb.store (maxGrDb, std::memory_order_release);
}

} // namespace cuesampler
