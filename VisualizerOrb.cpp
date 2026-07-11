#include "VisualizerOrb.h"
#include "CueFxRack/Parameters.h"

namespace cue
{

using namespace juce::gl;

//==============================================================================
// Shaders (GLSL 1.20 style; JUCE translates for GL3 contexts)
//==============================================================================
static const char* kVertexShader = R"(
    attribute vec2 position;
    varying vec2 vPos;
    void main()
    {
        vPos = position;
        gl_Position = vec4 (position, 0.0, 1.0);
    }
)";

static const char* kFragmentShader = R"(
    varying vec2 vPos;

    uniform float uTime;
    uniform float uLevel;
    uniform float uAspect;
    uniform vec4 uEQ;      // band gains -1..1 (MUD, BOXY, BITE, AIR)
    uniform vec4 uEQF;     // band freq positions 0..1
    uniform vec4 uComp;    // squash, attackSlow, thrust, oldMode
    uniform vec4 uLim;     // drive, ceilingAmt, release, -
    uniform vec4 uVerb;    // mix, size, decay, damp
    uniform vec4 uDly;     // mix, time, feedback, pingpong
    uniform vec4 uCrush;   // bitsAmt, rateAmt, mix, -
    uniform vec4 uImg;     // width 0..2, xover, bassMono, -
    uniform vec4 uMaster;  // inTrim01, outTrim01, -, -
    uniform vec4 uOn1;     // eq, comp, lim, verb
    uniform vec4 uOn2;     // dly, crush, img, -
    uniform vec4 uRip[8];  // pole dir xyz, age
    uniform float uRipS[8];
    uniform float uSpin;   // accumulated rotation (personality-paced)
    uniform float uRadius; // orb radius (master in trim)
    uniform vec4 uCenter;  // orb position offset (wander/orbit/twitch/gravity)
    uniform vec4 uLife;    // heaviness, agitation, -, -
    uniform vec4 uGhostA;  // delay ghost offset xyz + alpha
    uniform vec4 uGhostB;
    uniform float uPalette; // 0 = CUE warm, 1 = HALFTIME ice blue
    uniform vec4 uBg;       // backdrop colour behind the orb

    mat3 rotY (float a)
    {
        float s = sin (a), c = cos (a);
        return mat3 (c, 0.0, -s,  0.0, 1.0, 0.0,  s, 0.0, c);
    }

    mat3 rotX (float a)
    {
        float s = sin (a), c = cos (a);
        return mat3 (1.0, 0.0, 0.0,  0.0, c, s,  0.0, -s, c);
    }

    float n3 (vec3 p)   { return sin (p.x) * sin (p.y) * sin (p.z); }

    float fbm (vec3 p)
    {
        return 0.60 * n3 (p)
             + 0.30 * n3 (p * 2.13 + 1.7)
             + 0.15 * n3 (p * 4.37 + 4.2);
    }

    // Deformation field: the whole rack lives in here.
    float disp (vec3 n, float t)
    {
        // BIT CRUSHER: sample-rate reduction gently steps the motion clock
        float rateHz = 24.0 - 20.0 * uCrush.y;
        float tc = mix (t, floor (t * rateHz) / rateHz, uOn2.y * uCrush.z * (0.10 + 0.30 * uCrush.y));

        vec3 dA = normalize (vec3 ( 0.80,  0.32, -0.50));
        vec3 dB = normalize (vec3 (-0.42,  0.90,  0.20));
        vec3 dC = normalize (vec3 ( 0.10, -0.62,  0.78));
        vec3 dD = normalize (vec3 (-0.70, -0.20, -0.68));

        // idle life: barely-there swell, the orb rests as a near-sphere
        float form = 0.013 * sin (dot (n, dA) * 3.0 + tc * 0.8)
                   + 0.009 * sin (dot (n, dB) * 5.0 - tc * 0.6 + 1.3);

        // EQ: four spatial bands, SWELL lobes -> SHIMMER ripples
        float b1 = sin (dot (n, dA) * (2.0 + 2.0 * uEQF.x) + tc * 0.7);
        float b2 = sin (dot (n, dB) * (4.5 + 2.5 * uEQF.y) - tc * 0.9 + 1.7);
        float b3 = sin (dot (n, dC) * (8.0 + 4.0 * uEQF.z) + tc * 1.4 + 3.1);
        float b4 = sin (dot (n, dD) * (14.0 + 7.0 * uEQF.w) - tc * 1.9 + 5.2);

        // Thrust calms the low-lobe energy (like its sidechain LF cut)
        float thrustCalm = 1.0 - 0.55 * uComp.z * uOn1.y;
        form += uOn1.x * 0.12 * (uEQ.x * b1 * thrustCalm + uEQ.y * b2 + 0.8 * uEQ.z * b3 + 0.6 * uEQ.w * b4);

        // the shape blooms with the audio level, calm sphere when silent
        float d = form * (0.38 + 0.85 * uLevel);

        // audio breath
        d += uLevel * 0.09 * sin (dot (n, dB) * 6.0 + t * 7.0);

        // touch ripples expanding from module poles (always full strength)
        for (int i = 0; i < 8; ++i)
        {
            float amp = uRipS[i] * exp (-uRip[i].w * 2.2);
            if (amp < 0.004)
                continue;                                    // skip spent ripples
            float ang  = acos (clamp (dot (n, uRip[i].xyz), -1.0, 1.0));
            float ring = exp (-pow ((ang - uRip[i].w * 2.6) * 4.0, 2.0));
            d += amp * 0.13 * ring;
        }

        // COMPRESSOR: transient bulges escape a slow attack (audio-driven)
        float pulse = pow (max (0.0, sin (t * 1.35)), 6.0);
        d += uOn1.y * uComp.y * 0.09 * pulse * (0.15 + 0.85 * uLevel) * sin (dot (n, dC) * 3.0 + t);

        // ...then the squash compresses the deformation's dynamic range
        float knee = mix (6.0, 2.5, uComp.w);          // OLD (feedback) = softer
        d = d / (1.0 + uOn1.y * uComp.x * knee * abs (d));

        // LIMITER: hard ceiling shell flat-tops whatever pokes through
        float ceilAmt = mix (0.30, 0.09, uLim.y);
        d = mix (d, min (d, ceilAmt), uOn1.z);

        // BIT CRUSHER: terrace the surface into quantized steps
        float steps = mix (26.0, 4.0, uCrush.x);
        d = mix (d, floor (d * steps) / steps, uOn2.y * uCrush.z);

        return d;
    }

    float map (vec3 p, float t)
    {
        p -= uCenter.xyz;
        vec3 q = rotX (0.45 + 0.20 * sin (uTime * 0.21)) * (rotY (uSpin) * p);

        float d = disp (normalize (q), t);

        // heaviness: bass-weighted patches carry their mass low
        float down = clamp (-p.y / max (length (p), 1.0e-4), 0.0, 1.0);
        d *= 1.0 + uLife.x * 0.45 * down;

        return length (p) - (uRadius + d);
    }

    void main()
    {
        vec2 uv = vPos;
        uv.x *= uAspect;

        vec3 ro = vec3 (0.0, 0.0, -2.2);
        vec3 rd = normalize (vec3 (uv * 0.62, 1.0));

        float t = 0.0, minD = 1e9, hitT = -1.0;
        for (int i = 0; i < 48; ++i)
        {
            vec3 p = ro + rd * t;
            float d = map (p, uTime);
            minD = min (minD, d);
            if (d < 0.0025) { hitT = t; break; }
            t += max (d * 0.9, 0.005);
            if (t > 4.0) break;
        }

        // warm CUE palette, sliding to frozen ice blue when HALFTIME runs
        vec3 orange = mix (vec3 (0.886, 0.345, 0.169), vec3 (0.400, 0.660, 0.880), uPalette);
        vec3 deep   = mix (vec3 (0.620, 0.205, 0.098), vec3 (0.160, 0.360, 0.580), uPalette);
        vec3 peach  = mix (vec3 (0.955, 0.700, 0.560), vec3 (0.720, 0.860, 0.950), uPalette);
        vec3 white  = mix (vec3 (0.995, 0.940, 0.880), vec3 (0.920, 0.970, 1.000), uPalette);
        vec3 teal   = vec3 (0.440, 0.760, 0.660);   // reverb atmosphere
        vec3 violet = vec3 (0.610, 0.530, 0.870);   // delay echoes
        vec3 bg     = uBg.rgb;

        vec3 col = bg;

        if (hitT > 0.0)
        {
            vec3 p = ro + rd * hitT;
            vec2 e = vec2 (0.0045, 0.0);
            vec3 nrm = normalize (vec3 (map (p + e.xyy, uTime) - map (p - e.xyy, uTime),
                                        map (p + e.yxy, uTime) - map (p - e.yxy, uTime),
                                        map (p + e.yyx, uTime) - map (p - e.yyx, uTime)));

            float facing = max (0.0, dot (nrm, -rd));
            float fres   = pow (1.0 - facing, 2.5);

            // smokey marbling: broad and soft at rest, livelier with audio
            vec3 q = rotY (uSpin) * (p - uCenter.xyz);
            float smoke = 0.5 + 0.5 * fbm (q * 2.0 + vec3 (0.0, -uTime * 0.16, 0.0));
            smoke = mix (smoke, 0.5 + 0.5 * fbm (q * 1.4), uVerb.w * uOn1.w * 0.6);

            // vertical orange->peach gradient (logo), smoke pulls it whiter
            vec3 base = mix (peach, orange, clamp (0.42 + 1.1 * p.y, 0.0, 1.0));
            base = mix (base, deep,  0.35 * clamp (-p.y * 1.6, 0.0, 1.0));
            base = mix (base, white, smoke * (0.14 + 0.16 * uLevel + 0.12 * uVerb.x * uOn1.w));

            vec3 L = normalize (vec3 (-0.48, -0.62, -0.62));
            float key = max (0.0, dot (nrm, L));

            col = base * (0.34 + 0.80 * key);
            col += white * 0.30 * pow (key, 9.0);                     // hot spot
            col += white * fres * (0.22 + 0.30 * uVerb.x * uOn1.w + 0.20 * uLevel);   // rim glow

            // STEREO IMAGER: warm/cool lateral split of the shading
            float w = (uImg.x - 1.0) * uOn2.z;
            float split = nrm.x * w;
            col += vec3 (0.10, 0.015, -0.045) * split * 2.2;

            // DELAY: echo shells of the orb's past shape, etched as rings
            for (int k = 1; k <= 2; ++k)
            {
                float dtG   = (0.18 + 0.55 * uDly.y) * float (k);
                float ghost = map (p, uTime - dtG);
                float rim   = exp (-abs (ghost) * 46.0);
                col += violet * rim * uOn2.x * uDly.x * pow (uDly.z, float (k)) * 0.9;
            }
        }
        else
        {
            // warm halo, breathing with the audio
            float glow = 1.0 / (1.0 + minD * minD * 260.0);
            col += orange * glow * (0.18 + 0.55 * uLevel);
            col += white * glow * glow * 0.12;

            // REVERB builds the atmosphere: teal fog drifting around the orb.
            // SIZE extends its reach, DECAY thickens it, MIX sets density.
            float verbAmt = uVerb.x * uOn1.w;
            if (verbAmt > 0.001)
            {
                float fogN = 0.5 + 0.5 * fbm (vec3 (uv * 2.3, 0.7)
                                              + vec3 (0.0, -uTime * 0.06, uTime * 0.04));
                float reach = 1.0 / (1.0 + minD * minD * (34.0 - 26.0 * uVerb.y));
                col += teal  * fogN * reach * verbAmt * (0.40 + 0.40 * uVerb.z);
                col += white * fogN * reach * verbAmt * 0.10;
            }

            // DELAY populates the environment: violet ghost orbs trailing
            // behind on the orbit path
            vec3 gcA = uCenter.xyz + uGhostA.xyz;
            vec3 ocA = ro - gcA;
            float bA = dot (ocA, rd);
            float perpA = length (ocA - bA * rd);
            col += violet * exp (-abs (perpA - uRadius * 0.92) * 26.0) * uGhostA.w;

            vec3 gcB = uCenter.xyz + uGhostB.xyz;
            vec3 ocB = ro - gcB;
            float bB = dot (ocB, rd);
            float perpB = length (ocB - bB * rd);
            col += violet * exp (-abs (perpB - uRadius * 0.92) * 26.0) * uGhostB.w * 0.7;
        }

        // OUTPUT trim = exposure
        col *= 0.72 + 0.55 * uMaster.y;
        col = pow (col, vec3 (0.92));

        gl_FragColor = vec4 (col, 1.0);
    }
)";

//==============================================================================
VisualizerOrb::VisualizerOrb (juce::AudioProcessorValueTreeState& state)
    : apvts (state)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (true);

    cacheParameterPointers();

    lastFrameMs = juce::Time::getMillisecondCounterHiRes();

    context.setRenderer (this);
    context.setComponentPaintingEnabled (false);   // pure GL layer: no per-frame UI compositing (fixes window-drag stutter)

    // Timer-driven rendering (not continuous/vsync): frames are requested at
    // 30 Hz from the message thread, which avoids the GL-thread/UI-thread
    // contention that causes periodic micro-freezes on macOS.
    context.setContinuousRepainting (false);
    context.attachTo (*this);
    startTimerHz (30);
}

void VisualizerOrb::timerCallback()
{
    if (isShowing())
        context.triggerRepaint();
}

VisualizerOrb::~VisualizerOrb()
{
    stopTimer();
    context.detach();

    for (const auto& id : listenedIDs)
        apvts.removeParameterListener (id, this);
}

void VisualizerOrb::setLevel (float newLevel)
{
    targetLevel.store (juce::jlimit (0.0f, 1.0f, newLevel));
}

void VisualizerOrb::setHalfTimeActive (bool active)
{
    const auto v = active ? 1.0f : 0.0f;
    if (std::abs (externalHalfTime.load() - v) > 0.5f)
    {
        externalHalfTime.store (v);
        editCount[7].fetch_add (1);   // ripple from the master pole
    }
}

void VisualizerOrb::setBackgroundColour (juce::Colour backdrop)
{
    bgArgb.store (backdrop.getARGB());
}

//==============================================================================
void VisualizerOrb::cacheParameterPointers()
{
    static const char* ids[] = {
        pid::eqOn, pid::eqB1Freq, pid::eqB1Gain, pid::eqB2Freq, pid::eqB2Gain,
        pid::eqB3Freq, pid::eqB3Gain, pid::eqB4Freq, pid::eqB4Gain,
        pid::eqLfShelf, pid::eqHfShelf,
        pid::compOn, pid::compThresh, pid::compRatio, pid::compAttack, pid::compRelease,
        pid::compKnee, pid::compThrust, pid::compType, pid::compMakeup, pid::compMix,
        pid::limOn, pid::limGain, pid::limCeiling, pid::limRelease,
        pid::revOn, pid::revSize, pid::revDecay, pid::revDamp, pid::revPredelay,
        pid::revWidth, pid::revMix,
        pid::dlyOn, pid::dlyTime, pid::dlySync, pid::dlyDiv, pid::dlyFeedback,
        pid::dlyTone, pid::dlyPingPong, pid::dlyMix,
        pid::crushOn, pid::crushBits, pid::crushRate, pid::crushDrive, pid::crushMix,
        pid::imgOn, pid::imgWidth, pid::imgBassMono, pid::imgXover,
        pid::htOn, pid::htMix,
        pid::masterIn, pid::masterOut };

    for (auto* id : ids)
    {
        if (auto* raw = apvts.getRawParameterValue (id))
        {
            params[id] = raw;
            apvts.addParameterListener (id, this);
            listenedIDs.add (id);
        }
    }
}

float VisualizerOrb::raw (const char* paramID) const
{
    const auto it = params.find (paramID);
    return it != params.end() ? it->second->load() : 0.0f;
}

int VisualizerOrb::moduleIndexForParam (const juce::String& id)
{
    if (id.startsWith ("eq_"))     return 0;
    if (id.startsWith ("comp_"))   return 1;
    if (id.startsWith ("lim_"))    return 2;
    if (id.startsWith ("rev_"))    return 3;
    if (id.startsWith ("dly_"))    return 4;
    if (id.startsWith ("crush_"))  return 5;
    if (id.startsWith ("img_"))    return 6;
    return 7;                                    // master
}

void VisualizerOrb::parameterChanged (const juce::String& parameterID, float)
{
    editCount[moduleIndexForParam (parameterID)].fetch_add (1);   // RT-safe
}

//==============================================================================
void VisualizerOrb::newOpenGLContextCreated()
{
    shader = std::make_unique<juce::OpenGLShaderProgram> (context);

    shaderOk = shader->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (kVertexShader))
            && shader->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (kFragmentShader))
            && shader->link();

    if (! shaderOk)
    {
        DBG ("CUE ORB shader error: " + shader->getLastError());
        shader.reset();

        // fall back to the software-painted orb
        juce::Component::SafePointer<VisualizerOrb> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
            {
                safe->context.detach();
                safe->repaint();
            }
        });
    }

    static const float quad[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glGenBuffers (1, &quadVBO);
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
}

void VisualizerOrb::openGLContextClosing()
{
    shader.reset();
    if (quadVBO != 0)
    {
        glDeleteBuffers (1, &quadVBO);
        quadVBO = 0;
    }
}

void VisualizerOrb::renderOpenGL()
{
    const auto scale = (float) context.getRenderingScale();
    glViewport (0, 0, juce::roundToInt (scale * (float) getWidth()),
                      juce::roundToInt (scale * (float) getHeight()));

    const juce::Colour backdrop { bgArgb.load() };
    juce::OpenGLHelpers::clear (backdrop);

    if (! shaderOk || shader == nullptr)
        return;

    // --- animation clock + audio envelope --------------------------------
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto dt    = (float) juce::jlimit (0.0, 0.1, (nowMs - lastFrameMs) * 0.001);
    lastFrameMs      = nowMs;

    const auto tgt  = targetLevel.load();
    const auto rate = tgt > level ? 14.0f : 2.2f;
    level += (tgt - level) * juce::jmin (1.0f, rate * dt);

    // --- ripples ----------------------------------------------------------
    static const float poles[numModules][3] = {
        {  0.00f,  0.85f,  0.53f }, {  0.81f,  0.31f,  0.50f },
        {  0.50f, -0.69f,  0.53f }, { -0.50f, -0.69f,  0.53f },
        { -0.81f,  0.31f,  0.50f }, {  0.31f,  0.50f, -0.81f },
        { -0.31f, -0.50f, -0.81f }, {  0.00f,  0.00f, -1.00f } };

    float ripData[numModules * 4];
    float ripStrength[numModules];

    for (int m = 0; m < numModules; ++m)
    {
        const auto c = editCount[m].load();
        if (c != seenCount[m])
        {
            rippleStrength[m] = juce::jmin (1.6f, rippleStrength[m] * 0.4f + 0.55f * (float) (c - seenCount[m]));
            rippleAge[m] = 0.0f;
            seenCount[m] = c;
        }
        else
        {
            rippleAge[m] = juce::jmin (10.0f, rippleAge[m] + dt);
        }

        ripData[m * 4 + 0] = poles[m][0];
        ripData[m * 4 + 1] = poles[m][1];
        ripData[m * 4 + 2] = poles[m][2];
        ripData[m * 4 + 3] = rippleAge[m];
        ripStrength[m]     = rippleStrength[m];
    }

    // --- fingerprint: normalize every parameter --------------------------
    auto n01 = [] (float v, float lo, float hi) { return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo)); };

    const auto eq1 = raw (pid::eqB1Gain) / 12.0f, eq2 = raw (pid::eqB2Gain) / 12.0f;
    const auto eq3 = raw (pid::eqB3Gain) / 12.0f, eq4 = raw (pid::eqB4Gain) / 12.0f;
    const auto ef1 = raw (pid::eqB1Freq) / 6.0f,  ef2 = raw (pid::eqB2Freq) / 6.0f;
    const auto ef3 = raw (pid::eqB3Freq) / 6.0f,  ef4 = raw (pid::eqB4Freq) / 6.0f;

    const auto compSquash = 0.5f * n01 (raw (pid::compRatio), 0.0f, 5.0f)
                          + 0.5f * n01 (-raw (pid::compThresh), -10.0f, 40.0f);
    const auto compAtk    = raw (pid::compAttack) / 6.0f;
    const auto thrust     = raw (pid::compThrust) / 2.0f;
    const auto oldMode    = raw (pid::compType);

    const auto limDrive   = n01 (raw (pid::limGain), 0.0f, 24.0f);
    const auto limCeil    = juce::jlimit (0.0f, 1.0f, 0.45f * limDrive + 0.75f * n01 (-raw (pid::limCeiling), 0.0f, 20.0f));
    const auto limRel     = n01 (raw (pid::limRelease), 1.0f, 1000.0f);

    // ---- personality: the patch shapes the orb's temperament ------------
    const auto onEq    = raw (pid::eqOn);
    const auto onComp  = raw (pid::compOn);
    const auto onDly   = raw (pid::dlyOn);
    const auto onCrush = raw (pid::crushOn);

    // spectral tilt: bass boosts make it heavy and slow, treble makes it quick
    const auto eqTilt    = onEq * 0.5f * ((eq3 + eq4) - (eq1 + eq2));
    const auto heaviness = juce::jlimit (-1.0f, 1.0f, -eqTilt);
    const auto bitsAmt   = (16.0f - raw (pid::crushBits)) / 15.0f;
    const auto crushAmt  = onCrush * raw (pid::crushMix) * 0.01f * (0.3f + 0.7f * bitsAmt);
    const auto agitation = juce::jlimit (0.0f, 1.5f,
                               0.6f * juce::jmax (0.0f, eqTilt) + 0.7f * crushAmt + 0.3f * level);
    const auto tension   = juce::jlimit (0.0f, 1.0f, onComp * compSquash);   // comp disciplines it

    // clocks pace themselves to temperament
    animTime += dt * (1.0f + 0.6f * level) * (1.0f + 0.30f * agitation)
                   * (1.0f - 0.25f * juce::jmax (0.0f, heaviness));
    spin     += dt * 0.35f * (1.0f + 0.55f * agitation)
                   * (1.0f - 0.40f * juce::jmax (0.0f, heaviness));

    // orb stays locked to centre; motion is expressed in rotation/deformation
    const auto cx = 0.0f, cy = 0.0f;
    juce::ignoreUnused (tension);

    // delay ghosts circle the stationary orb; time sets the period
    const auto dlyAmt   = onDly * raw (pid::dlyMix) * 0.01f * (0.3f + 0.7f * raw (pid::dlyFeedback) / 95.0f);
    const auto orbitSec = juce::jlimit (0.25f, 3.0f, raw (pid::dlyTime) * 0.001f * 2.0f);
    orbitPhase += dt * juce::MathConstants<float>::twoPi / orbitSec;

    const auto ghostAX = 0.34f * std::cos (orbitPhase);
    const auto ghostAY = 0.24f * std::sin (orbitPhase);
    const auto ghostBX = 0.46f * std::cos (orbitPhase + juce::MathConstants<float>::pi);
    const auto ghostBY = 0.30f * std::sin (orbitPhase + juce::MathConstants<float>::pi);
    const auto ghostAlpha = dlyAmt * 0.85f;

    const auto orbRadius = 0.60f * (0.78f + 0.44f * n01 (raw (pid::masterIn), -24.0f, 24.0f));

    // HALFTIME freezes the orb's colour to ice blue (smoothly)
    const auto paletteTarget = (raw (pid::htOn) > 0.5f || externalHalfTime.load() > 0.5f) ? 1.0f : 0.0f;
    paletteBlend += (paletteTarget - paletteBlend) * juce::jmin (1.0f, 5.0f * dt);

    shader->use();

    auto set4 = [this] (const char* name, float a, float b, float c, float d)
    {
        if (auto loc = glGetUniformLocation (shader->getProgramID(), name); loc >= 0)
            glUniform4f (loc, a, b, c, d);
    };
    auto set1 = [this] (const char* name, float v)
    {
        if (auto loc = glGetUniformLocation (shader->getProgramID(), name); loc >= 0)
            glUniform1f (loc, v);
    };

    set1 ("uTime", animTime);
    set1 ("uLevel", level);
    set1 ("uAspect", (float) getWidth() / juce::jmax (1.0f, (float) getHeight()));
    set1 ("uSpin", spin);
    set1 ("uRadius", orbRadius);
    set4 ("uCenter", cx, cy, 0.0f, 0.0f);
    set4 ("uLife", heaviness, agitation, 0.0f, 0.0f);
    set4 ("uGhostA", ghostAX, ghostAY, 0.10f, ghostAlpha);
    set4 ("uGhostB", ghostBX, ghostBY, 0.20f, ghostAlpha * 0.7f);
    set1 ("uPalette", paletteBlend);
    set4 ("uBg", backdrop.getFloatRed(), backdrop.getFloatGreen(), backdrop.getFloatBlue(), 1.0f);

    set4 ("uEQ",  eq1, eq2, eq3, eq4);
    set4 ("uEQF", ef1, ef2, ef3, ef4);
    set4 ("uComp", compSquash, compAtk, thrust, oldMode);
    set4 ("uLim",  limDrive, limCeil, limRel, 0.0f);
    set4 ("uVerb", raw (pid::revMix) / 100.0f, raw (pid::revSize) / 100.0f,
                   n01 (raw (pid::revDecay), 0.1f, 15.0f), raw (pid::revDamp) / 100.0f);
    set4 ("uDly",  raw (pid::dlyMix) / 100.0f, n01 (raw (pid::dlyTime), 1.0f, 2000.0f),
                   raw (pid::dlyFeedback) / 95.0f, raw (pid::dlyPingPong));
    set4 ("uCrush", (16.0f - raw (pid::crushBits)) / 15.0f, n01 (raw (pid::crushRate), 1.0f, 50.0f),
                    raw (pid::crushMix) / 100.0f, 0.0f);
    set4 ("uImg",  raw (pid::imgWidth) / 100.0f, n01 (raw (pid::imgXover), 60.0f, 500.0f),
                   raw (pid::imgBassMono), 0.0f);
    set4 ("uMaster", n01 (raw (pid::masterIn), -24.0f, 24.0f),
                     n01 (raw (pid::masterOut), -24.0f, 24.0f), 0.0f, 0.0f);
    set4 ("uOn1", raw (pid::eqOn), raw (pid::compOn), raw (pid::limOn), raw (pid::revOn));
    set4 ("uOn2", raw (pid::dlyOn), raw (pid::crushOn), raw (pid::imgOn), 1.0f);

    if (auto loc = glGetUniformLocation (shader->getProgramID(), "uRip"); loc >= 0)
        glUniform4fv (loc, numModules, ripData);
    if (auto loc = glGetUniformLocation (shader->getProgramID(), "uRipS"); loc >= 0)
        glUniform1fv (loc, numModules, ripStrength);

    // --- fullscreen quad ---------------------------------------------------
    const auto posAttr = glGetAttribLocation (shader->getProgramID(), "position");
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    glEnableVertexAttribArray ((GLuint) posAttr);
    glVertexAttribPointer ((GLuint) posAttr, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray ((GLuint) posAttr);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
}

//==============================================================================
void VisualizerOrb::paint (juce::Graphics& g)
{
    // Software fallback (only visible if the GL shader failed): static orb.
    if (shaderOk)
        return;

    const auto b = getLocalBounds().toFloat();
    const auto r = juce::jmin (b.getWidth(), b.getHeight()) * 0.38f;
    const auto c = b.getCentre();

    if (r < 4.0f)
        return;

    g.fillAll (juce::Colour { bgArgb.load() });

    juce::ColourGradient grad (juce::Colour (0xffe2532e), c.x - r * 0.4f, c.y - r * 0.6f,
                               juce::Colour (0xffefa98c), c.x + r * 0.7f, c.y + r * 0.9f, true);
    g.setGradientFill (grad);
    g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
}

} // namespace cue
