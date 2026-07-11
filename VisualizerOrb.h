#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_opengl/juce_opengl.h>
#include <atomic>

namespace cue
{
    // ========================================================================
    // THE CUE ORB — CUERACK's UI/VisualizerOrb, ported whole.
    //
    // A GPU-raymarched, orange-white smokey orb that is a live physical model
    // of the FX rack:
    //
    //   EQ bands   -> four spatial deformation bands (MUD lobes ... AIR ripples)
    //   Compressor -> squashes deformation dynamic range; slow attack lets
    //                 transient bulges escape; Thrust calms the low lobes
    //   Limiter    -> hard ceiling shell that flat-tops spikes
    //   Reverb     -> atmosphere: smoke density + glow halo
    //   Delay      -> ghost echoes of the orb's own past shape
    //   Crusher    -> terraces the surface + stutters its motion clock
    //   Imager     -> warm/cool chromatic split of the shading
    //
    // The resting shape is therefore a visual fingerprint of the preset.
    // Touching any parameter fires a ripple wave from that module's pole.
    // setLevel() (fed from the sampler's output meter) drives breathing.
    //
    // Sampler-specific deltas from the rack original: the ray-miss backdrop
    // arrives as a uniform (uBg) so the orb composites onto the sampler's
    // themeable background, and setHalfTimeActive() lets the transport's
    // HALF TIME freeze the palette (the rack reads its own ht_on param).
    // ========================================================================
    class VisualizerOrb final : public juce::Component,
                                private juce::OpenGLRenderer,
                                private juce::Timer,
                                private juce::AudioProcessorValueTreeState::Listener
    {
    public:
        explicit VisualizerOrb (juce::AudioProcessorValueTreeState& state);
        ~VisualizerOrb() override;

        void setLevel (float newLevel);          // audio-thread safe, 0..1
        void setHalfTimeActive (bool active);    // transport HALF TIME -> ice palette
        void setBackgroundColour (juce::Colour backdrop);

        void paint (juce::Graphics&) override;   // software fallback only

    private:
        //==================================================================
        void newOpenGLContextCreated() override;
        void renderOpenGL() override;
        void openGLContextClosing() override;
        void timerCallback() override;

        void parameterChanged (const juce::String& parameterID, float newValue) override;

        void cacheParameterPointers();
        float raw (const char* paramID) const;   // current plain value of a param

        static int moduleIndexForParam (const juce::String& parameterID);

        //==================================================================
        juce::AudioProcessorValueTreeState& apvts;
        juce::OpenGLContext context;

        std::unique_ptr<juce::OpenGLShaderProgram> shader;
        GLuint quadVBO = 0;
        std::atomic<bool> shaderOk { false };

        std::map<juce::String, std::atomic<float>*> params;
        juce::StringArray listenedIDs;

        // Touch-ripple state: message/audio thread bumps a counter per module,
        // the GL thread turns counter changes into ripples.
        static constexpr int numModules = 8;     // eq comp lim verb dly crush img master
        std::atomic<juce::uint32> editCount[numModules] {};
        juce::uint32 seenCount[numModules] {};
        float rippleAge[numModules] {};
        float rippleStrength[numModules] {};

        std::atomic<float> targetLevel { 0.0f };
        std::atomic<float> externalHalfTime { 0.0f };
        std::atomic<juce::uint32> bgArgb { 0xff171412 };
        float level = 0.0f;
        float animTime = 0.0f;
        double lastFrameMs = 0.0;

        // personality motion state (GL thread only)
        float spin = 0.0f, orbitPhase = 0.0f, paletteBlend = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualizerOrb)
    };
}
