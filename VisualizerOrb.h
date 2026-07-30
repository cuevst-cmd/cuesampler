#pragma once

#include <juce_opengl/juce_opengl.h>
#include <atomic>

namespace cue
{
    // ========================================================================
    // THE CUE ORB — an audio-reactive sampler visual. The output meter drives
    // its breathing and the transport's HALF TIME state shifts its palette.
    // ========================================================================
    class VisualizerOrb final : public juce::Component,
                                private juce::OpenGLRenderer,
                                private juce::Timer,
                                private juce::ComponentListener
    {
    public:
        VisualizerOrb();
        ~VisualizerOrb() override;

        void setLevel (float newLevel);          // audio-thread safe, 0..1
        void setHalfTimeActive (bool active);    // transport HALF TIME -> ice palette
        void setBackgroundColour (juce::Colour backdrop);

        void paint (juce::Graphics&) override;   // software fallback only
        void parentHierarchyChanged() override;
        void componentMovedOrResized (juce::Component& component, bool wasMoved, bool wasResized) override;

    private:
        //==================================================================
        void newOpenGLContextCreated() override;
        void renderOpenGL() override;
        void openGLContextClosing() override;
        void timerCallback() override;

        juce::Component* observedTopLevel = nullptr;
        double lastMoveTime = 0.0;

        //==================================================================
        juce::OpenGLContext context;

        std::unique_ptr<juce::OpenGLShaderProgram> shader;
        GLuint quadVBO = 0;
        std::atomic<bool> shaderOk { false };

        // HALF TIME uses the final pole for a transport-state ripple. The
        // eight-pole layout stays fixed to keep the shader data compact.
        static constexpr int numModules = 8;
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

        // motion state (GL thread only)
        float spin = 0.0f, paletteBlend = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualizerOrb)
    };
}
