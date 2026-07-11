#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// ============================================================================
// CUE RACK parameter definitions.
// Chain (for the later DSP phase): IN > EQ > COMP > CRUSH > DELAY > REVERB
//                                  > IMAGER > LIMITER > OUT
// Stepped (choice) parameters mirror the API hardware switches — see
// docs/API_SOUND_RESEARCH.md.
// ============================================================================

namespace cue::pid
{
    // EQ (API 550B-style, 4 bands)
    inline constexpr auto eqOn        = "eq_on";
    inline constexpr auto eqB1Freq    = "eq_b1_freq";
    inline constexpr auto eqB1Gain    = "eq_b1_gain";
    inline constexpr auto eqB2Freq    = "eq_b2_freq";
    inline constexpr auto eqB2Gain    = "eq_b2_gain";
    inline constexpr auto eqB3Freq    = "eq_b3_freq";
    inline constexpr auto eqB3Gain    = "eq_b3_gain";
    inline constexpr auto eqB4Freq    = "eq_b4_freq";
    inline constexpr auto eqB4Gain    = "eq_b4_gain";
    inline constexpr auto eqLfShelf   = "eq_lf_shelf";
    inline constexpr auto eqHfShelf   = "eq_hf_shelf";

    // Compressor (API 2500-style)
    inline constexpr auto compOn      = "comp_on";
    inline constexpr auto compThresh  = "comp_thresh";
    inline constexpr auto compRatio   = "comp_ratio";
    inline constexpr auto compAttack  = "comp_attack";
    inline constexpr auto compRelease = "comp_release";
    inline constexpr auto compKnee    = "comp_knee";
    inline constexpr auto compThrust  = "comp_thrust";
    inline constexpr auto compType    = "comp_type";
    inline constexpr auto compMakeup  = "comp_makeup";
    inline constexpr auto compMix     = "comp_mix";

    // Limiter (multiband, L3-style)
    inline constexpr auto limOn       = "lim_on";
    inline constexpr auto limGain     = "lim_gain";
    inline constexpr auto limCeiling  = "lim_ceiling";
    inline constexpr auto limRelease  = "lim_release";
    inline constexpr auto limAutoRel  = "lim_autorel";   // ARC auto-release
    inline constexpr auto limTruePeak = "lim_truepeak";  // inter-sample peak ceiling

    // Reverb
    inline constexpr auto revOn       = "rev_on";
    inline constexpr auto revSize     = "rev_size";
    inline constexpr auto revDecay    = "rev_decay";
    inline constexpr auto revDamp     = "rev_damp";
    inline constexpr auto revPredelay = "rev_predelay";
    inline constexpr auto revWidth    = "rev_width";
    inline constexpr auto revMix      = "rev_mix";

    // Delay
    inline constexpr auto dlyOn       = "dly_on";
    inline constexpr auto dlyTime     = "dly_time";
    inline constexpr auto dlySync     = "dly_sync";
    inline constexpr auto dlyDiv      = "dly_div";
    inline constexpr auto dlyFeedback = "dly_feedback";
    inline constexpr auto dlyTone     = "dly_tone";
    inline constexpr auto dlyPingPong = "dly_pingpong";
    inline constexpr auto dlyMix      = "dly_mix";

    // Bit crusher
    inline constexpr auto crushOn     = "crush_on";
    inline constexpr auto crushBits   = "crush_bits";
    inline constexpr auto crushRate   = "crush_rate";
    inline constexpr auto crushDrive  = "crush_drive";
    inline constexpr auto crushMix    = "crush_mix";

    // Chorus
    inline constexpr auto chOn        = "ch_on";
    inline constexpr auto chRate      = "ch_rate";
    inline constexpr auto chDepth     = "ch_depth";
    inline constexpr auto chDelay     = "ch_delay";
    inline constexpr auto chFeedback  = "ch_feedback";
    inline constexpr auto chMix       = "ch_mix";

    // Flanger
    inline constexpr auto flOn        = "fl_on";
    inline constexpr auto flRate      = "fl_rate";
    inline constexpr auto flDepth     = "fl_depth";
    inline constexpr auto flFeedback  = "fl_feedback";
    inline constexpr auto flMix       = "fl_mix";

    // Flangus (multi-voice ensemble flanger)
    inline constexpr auto fgOn        = "fg_on";
    inline constexpr auto fgRate      = "fg_rate";
    inline constexpr auto fgDepth     = "fg_depth";
    inline constexpr auto fgSpread    = "fg_spread";
    inline constexpr auto fgMix       = "fg_mix";

    // Halftime
    inline constexpr auto htOn        = "ht_on";
    inline constexpr auto htSpeed     = "ht_speed";
    inline constexpr auto htWindow    = "ht_window";
    inline constexpr auto htSmooth    = "ht_smooth";
    inline constexpr auto htMix       = "ht_mix";
    inline constexpr auto htSync      = "ht_sync";      // tempo-sync the grain window
    inline constexpr auto htBars      = "ht_bars";      // window length in bars when synced

    // Guitar amp
    inline constexpr auto ampOn       = "amp_on";
    inline constexpr auto ampPreset   = "amp_preset";
    inline constexpr auto ampDrive    = "amp_drive";
    inline constexpr auto ampBass     = "amp_bass";
    inline constexpr auto ampMid      = "amp_mid";
    inline constexpr auto ampTreble   = "amp_treble";
    inline constexpr auto ampLevel    = "amp_level";
    inline constexpr auto ampMix      = "amp_mix";

    // Stereo imager
    inline constexpr auto imgOn       = "img_on";
    inline constexpr auto imgWidth    = "img_width";
    inline constexpr auto imgBassMono = "img_bassmono";
    inline constexpr auto imgXover    = "img_xover";

    // Master
    inline constexpr auto masterIn    = "master_in";
    inline constexpr auto masterOut   = "master_out";
}

namespace cue
{
    // Stepped frequency choices per EQ band (API 550B ranges)
    inline const juce::StringArray eqB1Freqs { "30", "40", "50", "100", "200", "300", "400" };          // LF, Hz
    inline const juce::StringArray eqB2Freqs { "75", "150", "180", "240", "500", "700", "1k" };         // LMF
    inline const juce::StringArray eqB3Freqs { "800", "1.5k", "2k", "3k", "4k", "5k", "6.5k" };         // HMF
    inline const juce::StringArray eqB4Freqs { "2.5k", "5k", "7k", "10k", "12.5k", "15k", "20k" };      // HF

    inline const juce::StringArray compRatios   { "1.5:1", "2:1", "3:1", "4:1", "6:1", "10:1", "LIM" };   // LIM = inf:1, like the hardware
    inline const juce::StringArray compAttacks  { ".03", ".1", ".3", "1", "3", "10", "30" };            // ms
    inline const juce::StringArray compKnees    { "HARD", "MED", "SOFT" };
    inline const juce::StringArray compThrusts  { "NORM", "MED", "LOUD" };
    inline const juce::StringArray compTypes    { "NEW", "OLD" };                                       // FF / FB
    inline const juce::StringArray delayDivs    { "1/1", "1/2", "1/4", "1/4.", "1/4T", "1/8", "1/8.", "1/8T", "1/16" };
    inline const juce::StringArray halftimeSpeeds { "1/2X", "1/4X" };
    inline const juce::StringArray halftimeBars  { "1/2", "1", "2", "4", "8" };   // bars, when synced

    // Voicings after famous amplifiers (evocative, not trademarked)
    inline const juce::StringArray ampPresets { "JZ CLEAN", "TWEED 57", "BASS 59",
                                                "BRIT PLEXI", "BRIT AC", "RECTO" };

    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        using namespace juce;
        using P     = AudioParameterFloat;
        using Pb    = AudioParameterBool;
        using Pc    = AudioParameterChoice;
        using Range = NormalisableRange<float>;

        auto dB   = AudioParameterFloatAttributes().withLabel ("dB");
        auto ms   = AudioParameterFloatAttributes().withLabel ("ms");
        auto sec  = AudioParameterFloatAttributes().withLabel ("s");
        auto hz   = AudioParameterFloatAttributes().withLabel ("Hz");
        auto pct  = AudioParameterFloatAttributes().withLabel ("%");

        AudioProcessorValueTreeState::ParameterLayout layout;

        // -------------------------------------------------- EQ
        layout.add (std::make_unique<Pb> (ParameterID { pid::eqOn, 1 },      "EQ On", true),
                    std::make_unique<Pc> (ParameterID { pid::eqB1Freq, 1 },  "EQ LF Freq",  eqB1Freqs, 3),
                    std::make_unique<P>  (ParameterID { pid::eqB1Gain, 1 },  "EQ LF Gain",  Range (-12.0f, 12.0f, 0.5f), 0.0f, dB),
                    std::make_unique<Pc> (ParameterID { pid::eqB2Freq, 1 },  "EQ LMF Freq", eqB2Freqs, 3),
                    std::make_unique<P>  (ParameterID { pid::eqB2Gain, 1 },  "EQ LMF Gain", Range (-12.0f, 12.0f, 0.5f), 0.0f, dB),
                    std::make_unique<Pc> (ParameterID { pid::eqB3Freq, 1 },  "EQ HMF Freq", eqB3Freqs, 3),
                    std::make_unique<P>  (ParameterID { pid::eqB3Gain, 1 },  "EQ HMF Gain", Range (-12.0f, 12.0f, 0.5f), 0.0f, dB),
                    std::make_unique<Pc> (ParameterID { pid::eqB4Freq, 1 },  "EQ HF Freq",  eqB4Freqs, 3),
                    std::make_unique<P>  (ParameterID { pid::eqB4Gain, 1 },  "EQ HF Gain",  Range (-12.0f, 12.0f, 0.5f), 0.0f, dB),
                    std::make_unique<Pb> (ParameterID { pid::eqLfShelf, 1 }, "EQ LF Shelf", false),
                    std::make_unique<Pb> (ParameterID { pid::eqHfShelf, 1 }, "EQ HF Shelf", false));

        // -------------------------------------------------- Compressor
        layout.add (std::make_unique<Pb> (ParameterID { pid::compOn, 1 },      "Comp On", false),
                    std::make_unique<P>  (ParameterID { pid::compThresh, 1 },  "Comp Threshold", Range (-40.0f, 10.0f, 0.1f), 0.0f, dB),
                    std::make_unique<Pc> (ParameterID { pid::compRatio, 1 },   "Comp Ratio",   compRatios, 2),
                    std::make_unique<Pc> (ParameterID { pid::compAttack, 1 },  "Comp Attack",  compAttacks, 3),
                    std::make_unique<P>  (ParameterID { pid::compRelease, 1 }, "Comp Release", Range (0.05f, 2.0f, 0.01f, 0.5f), 0.3f, sec),
                    std::make_unique<Pc> (ParameterID { pid::compKnee, 1 },    "Comp Knee",    compKnees, 1),
                    std::make_unique<Pc> (ParameterID { pid::compThrust, 1 },  "Comp Thrust",  compThrusts, 0),
                    std::make_unique<Pc> (ParameterID { pid::compType, 1 },    "Comp Type",    compTypes, 0),
                    std::make_unique<P>  (ParameterID { pid::compMakeup, 1 },  "Comp Makeup",  Range (-6.0f, 24.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::compMix, 1 },     "Comp Mix",     Range (0.0f, 100.0f, 1.0f), 100.0f, pct));

        // -------------------------------------------------- Limiter (multiband, L3-style)
        layout.add (std::make_unique<Pb> (ParameterID { pid::limOn, 1 },       "Limiter On", false),
                    std::make_unique<P>  (ParameterID { pid::limGain, 1 },     "Limiter Gain",    Range (0.0f, 24.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::limCeiling, 1 },  "Limiter Ceiling", Range (-20.0f, 0.0f, 0.1f), -0.3f, dB),
                    std::make_unique<P>  (ParameterID { pid::limRelease, 1 },  "Limiter Release", Range (1.0f, 1000.0f, 1.0f, 0.4f), 100.0f, ms),
                    std::make_unique<Pb> (ParameterID { pid::limAutoRel, 1 },  "Limiter Auto Release", true),
                    std::make_unique<Pb> (ParameterID { pid::limTruePeak, 1 }, "Limiter True Peak",    true));

        // -------------------------------------------------- Reverb
        layout.add (std::make_unique<Pb> (ParameterID { pid::revOn, 1 },       "Reverb On", false),
                    std::make_unique<P>  (ParameterID { pid::revSize, 1 },     "Reverb Size",      Range (0.0f, 100.0f, 1.0f), 50.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::revDecay, 1 },    "Reverb Decay",     Range (0.1f, 15.0f, 0.1f, 0.4f), 2.0f, sec),
                    std::make_unique<P>  (ParameterID { pid::revDamp, 1 },     "Reverb Damp",      Range (0.0f, 100.0f, 1.0f), 50.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::revPredelay, 1 }, "Reverb Pre-Delay", Range (0.0f, 200.0f, 1.0f), 20.0f, ms),
                    std::make_unique<P>  (ParameterID { pid::revWidth, 1 },    "Reverb Width",     Range (0.0f, 100.0f, 1.0f), 100.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::revMix, 1 },      "Reverb Mix",       Range (0.0f, 100.0f, 1.0f), 25.0f, pct));

        // -------------------------------------------------- Delay
        layout.add (std::make_unique<Pb> (ParameterID { pid::dlyOn, 1 },       "Delay On", false),
                    std::make_unique<P>  (ParameterID { pid::dlyTime, 1 },     "Delay Time",     Range (1.0f, 2000.0f, 1.0f, 0.35f), 350.0f, ms),
                    std::make_unique<Pb> (ParameterID { pid::dlySync, 1 },     "Delay Sync", false),
                    std::make_unique<Pc> (ParameterID { pid::dlyDiv, 1 },      "Delay Division", delayDivs, 2),
                    std::make_unique<P>  (ParameterID { pid::dlyFeedback, 1 }, "Delay Feedback", Range (0.0f, 95.0f, 1.0f), 35.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::dlyTone, 1 },     "Delay Tone",     Range (200.0f, 12000.0f, 1.0f, 0.3f), 6000.0f, hz),
                    std::make_unique<Pb> (ParameterID { pid::dlyPingPong, 1 }, "Delay Ping-Pong", false),
                    std::make_unique<P>  (ParameterID { pid::dlyMix, 1 },      "Delay Mix",      Range (0.0f, 100.0f, 1.0f), 25.0f, pct));

        // -------------------------------------------------- Bit crusher
        layout.add (std::make_unique<Pb> (ParameterID { pid::crushOn, 1 },    "Crusher On", false),
                    std::make_unique<P>  (ParameterID { pid::crushBits, 1 },  "Crusher Bits",  Range (1.0f, 16.0f, 0.5f), 16.0f),
                    std::make_unique<P>  (ParameterID { pid::crushRate, 1 },  "Crusher Rate",  Range (1.0f, 50.0f, 1.0f), 1.0f, AudioParameterFloatAttributes().withLabel ("x")),
                    std::make_unique<P>  (ParameterID { pid::crushDrive, 1 }, "Crusher Drive", Range (0.0f, 24.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::crushMix, 1 },   "Crusher Mix",   Range (0.0f, 100.0f, 1.0f), 100.0f, pct));

        // -------------------------------------------------- Chorus (off + toolbar by default)
        layout.add (std::make_unique<Pb> (ParameterID { pid::chOn, 1 },       "Chorus On", false),
                    std::make_unique<P>  (ParameterID { pid::chRate, 1 },     "Chorus Rate",     Range (0.05f, 5.0f, 0.01f, 0.5f), 0.8f, hz),
                    std::make_unique<P>  (ParameterID { pid::chDepth, 1 },    "Chorus Depth",    Range (0.0f, 100.0f, 1.0f), 35.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::chDelay, 1 },    "Chorus Delay",    Range (5.0f, 30.0f, 0.1f), 18.0f, ms),
                    std::make_unique<P>  (ParameterID { pid::chFeedback, 1 }, "Chorus Feedback", Range (-95.0f, 95.0f, 1.0f), 0.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::chMix, 1 },      "Chorus Mix",      Range (0.0f, 100.0f, 1.0f), 50.0f, pct));

        // -------------------------------------------------- Flanger (off + toolbar by default)
        layout.add (std::make_unique<Pb> (ParameterID { pid::flOn, 1 },       "Flanger On", false),
                    std::make_unique<P>  (ParameterID { pid::flRate, 1 },     "Flanger Rate",     Range (0.05f, 5.0f, 0.01f, 0.5f), 0.3f, hz),
                    std::make_unique<P>  (ParameterID { pid::flDepth, 1 },    "Flanger Depth",    Range (0.0f, 100.0f, 1.0f), 60.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::flFeedback, 1 }, "Flanger Feedback", Range (-95.0f, 95.0f, 1.0f), 40.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::flMix, 1 },      "Flanger Mix",      Range (0.0f, 100.0f, 1.0f), 50.0f, pct));

        // -------------------------------------------------- Flangus (off + toolbar by default)
        layout.add (std::make_unique<Pb> (ParameterID { pid::fgOn, 1 },     "Flangus On", false),
                    std::make_unique<P>  (ParameterID { pid::fgRate, 1 },   "Flangus Rate",   Range (0.05f, 3.0f, 0.01f, 0.5f), 0.4f, hz),
                    std::make_unique<P>  (ParameterID { pid::fgDepth, 1 },  "Flangus Depth",  Range (0.0f, 100.0f, 1.0f), 50.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::fgSpread, 1 }, "Flangus Spread", Range (0.0f, 100.0f, 1.0f), 70.0f, pct),
                    std::make_unique<P>  (ParameterID { pid::fgMix, 1 },    "Flangus Mix",    Range (0.0f, 100.0f, 1.0f), 50.0f, pct));

        // (CUESAMPLER: no Halftime module here — the transport's HALF TIME
        //  owns that job; the pid::ht* ids above stay for orb compatibility.)

        // -------------------------------------------------- Guitar amp (off + toolbar by default)
        layout.add (std::make_unique<Pb> (ParameterID { pid::ampOn, 1 },     "Amp On", false),
                    std::make_unique<Pc> (ParameterID { pid::ampPreset, 1 }, "Amp Preset", ampPresets, 1),
                    std::make_unique<P>  (ParameterID { pid::ampDrive, 1 },  "Amp Drive",  Range (0.0f, 36.0f, 0.1f), 12.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::ampBass, 1 },   "Amp Bass",   Range (-12.0f, 12.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::ampMid, 1 },    "Amp Mid",    Range (-12.0f, 12.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::ampTreble, 1 }, "Amp Treble", Range (-12.0f, 12.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::ampLevel, 1 },  "Amp Level",  Range (-24.0f, 12.0f, 0.1f), 0.0f, dB),
                    std::make_unique<P>  (ParameterID { pid::ampMix, 1 },    "Amp Mix",    Range (0.0f, 100.0f, 1.0f), 100.0f, pct));

        // -------------------------------------------------- Stereo imager
        layout.add (std::make_unique<Pb> (ParameterID { pid::imgOn, 1 },       "Imager On", false),
                    std::make_unique<P>  (ParameterID { pid::imgWidth, 1 },    "Imager Width",     Range (0.0f, 200.0f, 1.0f), 100.0f, pct),
                    std::make_unique<Pb> (ParameterID { pid::imgBassMono, 1 }, "Imager Bass Mono", false),
                    std::make_unique<P>  (ParameterID { pid::imgXover, 1 },    "Imager Crossover", Range (60.0f, 500.0f, 1.0f, 0.5f), 120.0f, hz));

        // (CUESAMPLER: no Master module — the sampler's own gain staging
        //  applies; pid::master* ids stay for orb compatibility.)

        return layout;
    }
}
