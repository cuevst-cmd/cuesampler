// Standalone unit tests for cuesampler::WarpMap. Build with:
//   c++ -std=c++20 -O2 -I.. tools/test_warp_map.cpp ../WarpMap.cpp -o test_warp_map
// Run with: ./test_warp_map
//
// No JUCE dependency on purpose — keep these runnable from any environment.
//
// =========================================================================
// Manual integration test checklist (step 13).
// Things below need the live plugin (JUCE + Bungee) and a real audio file.
// Run after every meaningful change to the warp pipeline.
//
// Pipeline smoke
//   [ ] Load a sample, wait for tempo analysis, click WARP toggle. Cursor
//       changes to crosshair inside the chop area.
//   [ ] Click inside a chop. A violet triangle appears at the click point.
//       The audio should re-bake within ~100 ms (no audible silence on
//       playback — falls back to source audio while baking).
//   [ ] Click again at a different transient inside the same chop. Both
//       markers visible. Connectors slant toward their snapped beat.
//   [ ] Disable WARP toggle. Cursor reverts. Click-to-preview works again.
//
// Drag interactions
//   [ ] Default drag of a marker in WARP mode: local-time-x slides along
//       the chop width, snapping to the active grid division. Connector
//       follows.
//   [ ] Shift-drag: snap magnet bypassed; marker moves freely. Triangle
//       paints with the normal violet (snappedToGrid == false).
//   [ ] Cmd/Ctrl-drag: triangle slides in source-x; local-time-x stays
//       put. The waveform region under the triangle changes.
//
// Right-click menu
//   [ ] Right-click a marker → menu with "Clear marker" + per-division
//       snap items. Each menu item produces the expected change. Click off
//       the menu dismisses without action.
//   [ ] Right-click empty space (no marker) → no menu opens.
//
// Stale tint
//   [ ] Place a snapped marker. Adjust the TEMPO trim knob. Marker
//       triangle turns amber, with a faint outline ring; connector also
//       turns amber.
//   [ ] Re-snap the marker (drag or right-click → snap). Tint reverts.
//   [ ] Repeat with a Shift-dragged (free) marker — should NOT turn stale
//       when grid changes.
//
// Persistence
//   [ ] Place several markers, save the project, close and reload. All
//       markers come back in the right places, with correct snappedToGrid
//       flag, and audio plays warped on first trigger.
//   [ ] Save with the WARP mode toggle on, reload — toggle defaults to
//       off (intentionally not persisted; document this in release notes).
//
// Re-analysis with markers
//   [ ] Place markers across a chop. Change BARS to a new value (e.g., 1
//       → 2). Chop boundaries change; markers that fall outside new chop
//       bounds are dropped, the rest survive. Cache is rebuilt.
//
// Extreme ratios
//   [ ] Drag a marker so a segment would exceed [0.25, 4.0] speed. The
//       move is rejected (marker doesn't move past the safe boundary).
//
// MIDI rapid-fire while baking
//   [ ] Load a long sample with a chop that has many markers (extends
//       bake time). Trigger the chop via MIDI repeatedly while bakes are
//       queued. Audio always plays — falls back to unwarped if the cache
//       isn't ready, never silent.
//
// Deep zoom
//   [ ] Zoom the waveform display fully in. Markers/triangles/connectors
//       remain visible; hit-test still picks the correct marker on click.
//
// Export drag
//   [ ] Hold-to-export a warped chop. The exported WAV contains the warp
//       baked in. Open the WAV in another tool to confirm.
// =========================================================================

#include "../WarpMap.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using cuesampler::ChopWarpMarker;
using cuesampler::WarpMap;

static int gFailures = 0;
static int gChecks   = 0;

static void check (bool ok, const char* expr, const char* test, int line)
{
    ++gChecks;
    if (! ok)
    {
        ++gFailures;
        std::fprintf (stderr, "  FAIL [%s:%d] %s    in test: %s\n", __FILE__, line, expr, test);
    }
}

static void checkClose (double a, double b, double tol, const char* expr, const char* test, int line)
{
    ++gChecks;
    if (! (std::fabs (a - b) <= tol))
    {
        ++gFailures;
        std::fprintf (stderr, "  FAIL [%s:%d] %s    expected ≈ %.9f got %.9f (tol %.1e)    in test: %s\n",
                      __FILE__, line, expr, b, a, tol, test);
    }
}

#define CHECK(cond)                check ((cond), #cond, currentTest, __LINE__)
#define CHECK_CLOSE(a, b, tol)     checkClose ((a), (b), (tol), #a " ≈ " #b, currentTest, __LINE__)

static const char* currentTest = "<none>";

// --- Tests -----------------------------------------------------------------

static void test_identity_no_markers()
{
    currentTest = "identity_no_markers";

    WarpMap m;
    m.build (1000, 5000, {}, 48000.0);

    CHECK (m.isIdentity());
    CHECK (m.numInternalMarkers() == 0);
    CHECK (m.numDroppedMarkers() == 0);
    CHECK_CLOSE (m.totalLocalDurationSeconds(), 4000.0 / 48000.0, 1e-9);

    // Endpoints
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.0), 1000.0, 1e-9);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (m.totalLocalDurationSeconds()), 5000.0, 1e-9);

    // Midpoint should be linear
    const double midLocal = m.totalLocalDurationSeconds() * 0.5;
    CHECK_CLOSE (m.sourceSampleAtLocalTime (midLocal), 3000.0, 1e-9);

    // Inverse round-trip
    CHECK_CLOSE (m.localTimeAtSourceSample (3000.0), midLocal, 1e-9);
}

static void test_endpoint_clamping()
{
    currentTest = "endpoint_clamping";

    WarpMap m;
    m.build (0, 4800, {}, 48000.0); // 0.1 s chop

    // Out-of-range queries clamp.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (-1.0), 0.0, 1e-9);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (10.0), 4800.0, 1e-9);
    CHECK_CLOSE (m.localTimeAtSourceSample (-100.0), 0.0, 1e-9);
    CHECK_CLOSE (m.localTimeAtSourceSample (10000.0), 0.1, 1e-9);
}

static void test_invalid_inputs()
{
    currentTest = "invalid_inputs";

    WarpMap m;

    // Zero-length chop
    m.build (1000, 1000, {}, 48000.0);
    CHECK (m.getNodes().empty());
    CHECK_CLOSE (m.totalLocalDurationSeconds(), 0.0, 1e-9);

    // Negative-length chop
    m.build (5000, 1000, {}, 48000.0);
    CHECK (m.getNodes().empty());

    // Zero sample rate
    m.build (0, 4800, {}, 0.0);
    CHECK (m.getNodes().empty());

    // Negative sample rate
    m.build (0, 4800, {}, -100.0);
    CHECK (m.getNodes().empty());
}

static void test_single_marker_identity_position()
{
    currentTest = "single_marker_at_natural_time";
    // Marker at sourceSample=2000 with localTime equal to its natural time
    // (i.e., still identity). Should be accepted, but produce identical output.
    const double sr = 48000.0;
    ChopWarpMarker m1;
    m1.sourceSample     = 2000;
    m1.localTimeSeconds = (2000 - 1000) / sr; // natural time

    WarpMap m;
    m.build (1000, 5000, { m1 }, sr);

    CHECK (! m.isIdentity());
    CHECK (m.numInternalMarkers() == 1);
    CHECK (m.numDroppedMarkers() == 0);

    // Both halves should still behave linearly.
    CHECK_CLOSE (m.sourceSampleAtLocalTime ((2000 - 1000) / sr), 2000.0, 1e-6);
    // Midpoint of first segment
    CHECK_CLOSE (m.sourceSampleAtLocalTime (((2000 - 1000) / sr) * 0.5), 1500.0, 1e-6);
    // Midpoint of second segment
    const double t2 = (2000 - 1000) / sr + ((5000 - 2000) / sr) * 0.5;
    CHECK_CLOSE (m.sourceSampleAtLocalTime (t2), 3500.0, 1e-6);
}

static void test_single_marker_warped()
{
    currentTest = "single_marker_warped";
    // Pull a transient earlier in playback time — first half is sped up,
    // second half is slowed down.
    const double sr = 48000.0;
    const int    chopStart = 0;
    const int    chopEnd   = 9600; // 0.2 s
    const int    src       = 4800; // 0.1 s natural
    const double targetLocal = 0.05; // play it at 0.05 s instead of 0.1 s

    ChopWarpMarker m1;
    m1.sourceSample     = src;
    m1.localTimeSeconds = targetLocal;

    WarpMap m;
    m.build (chopStart, chopEnd, { m1 }, sr);

    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 1);

    // At the marker's local time, source position must equal the marker.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (targetLocal), 4800.0, 1e-6);

    // Halfway through the first segment in local time, source is at 2400.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (targetLocal * 0.5), 2400.0, 1e-6);

    // Halfway through the second segment in local time, source is at 7200.
    const double secondMidLocal = targetLocal + (m.totalLocalDurationSeconds() - targetLocal) * 0.5;
    CHECK_CLOSE (m.sourceSampleAtLocalTime (secondMidLocal), 7200.0, 1e-6);

    // Inverse round-trip at the marker.
    CHECK_CLOSE (m.localTimeAtSourceSample (4800.0), targetLocal, 1e-6);
}

static void test_multiple_markers()
{
    currentTest = "multiple_markers";
    const double sr = 48000.0;
    ChopWarpMarker a; a.sourceSample = 2400;  a.localTimeSeconds = 0.04;  // 0.05 nat → 0.04
    ChopWarpMarker b; b.sourceSample = 7200;  b.localTimeSeconds = 0.12;  // 0.15 nat → 0.12

    WarpMap m;
    m.build (0, 9600, { a, b }, sr); // 0.2 s total

    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 2);

    // Each marker's local time maps back to its source.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.04), 2400.0, 1e-6);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.12), 7200.0, 1e-6);

    // Endpoints unchanged.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.0), 0.0, 1e-9);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.2), 9600.0, 1e-9);
}

static void test_speed_clamp_rejects_extreme_stretch()
{
    currentTest = "speed_clamp_rejects_extreme_stretch";
    // Try to make the first segment have speed = 0.1 (way below kMinSpeed=0.25):
    // sourceDuration = 480/48000 = 0.01 s, localDuration = 0.1 s → speed = 0.1.
    // Segment from 0..480 is 0.01 s of audio stretched to play in 0.1 s.
    const double sr = 48000.0;
    ChopWarpMarker m1;
    m1.sourceSample     = 480;
    m1.localTimeSeconds = 0.1;

    WarpMap m;
    m.build (0, 9600, { m1 }, sr); // 0.2 s chop

    CHECK (m.numDroppedMarkers() == 1);
    CHECK (m.isIdentity());
}

static void test_speed_clamp_rejects_extreme_compression()
{
    currentTest = "speed_clamp_rejects_extreme_compression";
    // Try to make the first segment have speed = 5.0 (above kMaxSpeed=4.0):
    // sourceDuration = 4800/48000 = 0.1 s, localDuration = 0.02 s → speed = 5.0.
    const double sr = 48000.0;
    ChopWarpMarker m1;
    m1.sourceSample     = 4800;
    m1.localTimeSeconds = 0.02;

    WarpMap m;
    m.build (0, 9600, { m1 }, sr);

    CHECK (m.numDroppedMarkers() == 1);
    CHECK (m.isIdentity());
}

static void test_marker_outside_chop_rejected()
{
    currentTest = "marker_outside_chop_rejected";
    const double sr = 48000.0;

    ChopWarpMarker before;  before.sourceSample = 100;   before.localTimeSeconds = 0.01;
    ChopWarpMarker after;   after.sourceSample  = 9999;  after.localTimeSeconds  = 0.18;
    ChopWarpMarker atStart; atStart.sourceSample = 0;    atStart.localTimeSeconds = 0.0;
    ChopWarpMarker atEnd;   atEnd.sourceSample = 9600;   atEnd.localTimeSeconds = 0.2;

    WarpMap m;
    m.build (200, 9600, { before, after, atStart, atEnd }, sr);

    CHECK (m.numInternalMarkers() == 0);
    CHECK (m.numDroppedMarkers() == 4);
    CHECK (m.isIdentity());
}

static void test_unsorted_markers_are_sorted()
{
    currentTest = "unsorted_markers_are_sorted";
    const double sr = 48000.0;
    ChopWarpMarker a; a.sourceSample = 7200; a.localTimeSeconds = 0.12;
    ChopWarpMarker b; b.sourceSample = 2400; b.localTimeSeconds = 0.04;

    WarpMap m;
    m.build (0, 9600, { a, b }, sr); // intentionally out of order

    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 2);
    CHECK (m.getNodes()[1].sourceSample == 2400.0);
    CHECK (m.getNodes()[2].sourceSample == 7200.0);
}

static void test_non_monotonic_local_time_rejected()
{
    currentTest = "non_monotonic_local_time_rejected";
    const double sr = 48000.0;
    // Both markers in a valid sourceSample order, but local times go backward.
    ChopWarpMarker a; a.sourceSample = 2400; a.localTimeSeconds = 0.12;
    ChopWarpMarker b; b.sourceSample = 7200; b.localTimeSeconds = 0.04;

    WarpMap m;
    m.build (0, 9600, { a, b }, sr);

    // First marker accepted, second rejected (local time would go back).
    CHECK (m.numInternalMarkers() == 1);
    CHECK (m.numDroppedMarkers() == 1);
}

static void test_round_trip_random()
{
    currentTest = "round_trip_random";
    // Build a multi-marker map and verify forward∘inverse = identity over a
    // sweep of source samples and local times.
    const double sr = 48000.0;
    ChopWarpMarker a; a.sourceSample = 1000; a.localTimeSeconds = 0.030;
    ChopWarpMarker b; b.sourceSample = 4000; b.localTimeSeconds = 0.075;
    ChopWarpMarker c; c.sourceSample = 7000; c.localTimeSeconds = 0.140;

    WarpMap m;
    m.build (0, 9600, { a, b, c }, sr);
    CHECK (m.numDroppedMarkers() == 0);

    for (int i = 0; i <= 200; ++i)
    {
        const double t = (m.totalLocalDurationSeconds() * i) / 200.0;
        const double s = m.sourceSampleAtLocalTime (t);
        const double tBack = m.localTimeAtSourceSample (s);
        CHECK_CLOSE (tBack, t, 1e-6);
    }

    for (int i = 0; i <= 200; ++i)
    {
        const double s = (9600.0 * i) / 200.0;
        const double t = m.localTimeAtSourceSample (s);
        const double sBack = m.sourceSampleAtLocalTime (t);
        CHECK_CLOSE (sBack, s, 1e-3);
    }
}

static void test_speed_clamp_boundary_accepted()
{
    currentTest = "speed_clamp_boundary_accepted";
    // Speed exactly at 0.25 should be accepted (allow the boundary).
    const double sr = 48000.0;
    // sourceDuration = 1200/48000 = 0.025 s, localDuration = 0.1 s → speed = 0.25
    ChopWarpMarker m1;
    m1.sourceSample     = 1200;
    m1.localTimeSeconds = 0.1;

    WarpMap m;
    m.build (0, 9600, { m1 }, sr);

    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 1);
}

// --- Render tests (renderWarpedChopLinear) --------------------------------

namespace render_tests
{
    // Build a deterministic source buffer where source[ch][i] = (ch+1) * (i+1).
    // Distinct per-channel values make off-by-channel bugs obvious.
    static std::vector<std::vector<float>> makeSource (int channels, int frames)
    {
        std::vector<std::vector<float>> src ((size_t) channels);
        for (int ch = 0; ch < channels; ++ch)
        {
            src[(size_t) ch].resize ((size_t) frames);
            for (int i = 0; i < frames; ++i)
                src[(size_t) ch][(size_t) i] = (float) ((ch + 1) * (i + 1));
        }
        return src;
    }

    static std::vector<const float*> rawConstPtrs (const std::vector<std::vector<float>>& v)
    {
        std::vector<const float*> ptrs (v.size(), nullptr);
        for (size_t i = 0; i < v.size(); ++i)
            ptrs[i] = v[i].data();
        return ptrs;
    }

    static std::vector<float*> rawPtrs (std::vector<std::vector<float>>& v)
    {
        std::vector<float*> ptrs (v.size(), nullptr);
        for (size_t i = 0; i < v.size(); ++i)
            ptrs[i] = v[i].data();
        return ptrs;
    }
}

static void test_render_identity_is_bit_exact()
{
    currentTest = "render_identity_is_bit_exact";
    const double sr = 48000.0;
    const int chopStart = 100;
    const int chopEnd   = 1100;     // 1000 frames
    const int channels  = 2;
    const int srcFrames = 4000;

    auto src = render_tests::makeSource (channels, srcFrames);
    const auto srcPtrs = render_tests::rawConstPtrs (src);

    WarpMap m;
    m.build (chopStart, chopEnd, {}, sr);
    const int frames = warpedChopFrameCount (m);
    CHECK (frames == 1000);

    std::vector<std::vector<float>> dst ((size_t) channels,
                                         std::vector<float> ((size_t) frames, 999.0f));
    auto dstPtrs = render_tests::rawPtrs (dst);

    renderWarpedChopLinear (m, srcPtrs.data(), srcFrames, channels,
                            dstPtrs.data(), frames);

    // Bit-exact match against source[chopStart..chopEnd).
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < frames; ++i)
            CHECK (dst[(size_t) ch][(size_t) i] == src[(size_t) ch][(size_t) (chopStart + i)]);
}

static void test_render_target_zeroed_when_oversized()
{
    currentTest = "render_target_zeroed_when_oversized";
    const double sr = 48000.0;
    const int chopStart = 0;
    const int chopEnd   = 480;       // 10 ms = 480 frames
    const int channels  = 1;

    auto src = render_tests::makeSource (channels, 10000);
    const auto srcPtrs = render_tests::rawConstPtrs (src);

    WarpMap m;
    m.build (chopStart, chopEnd, {}, sr);
    const int produced = warpedChopFrameCount (m);
    const int oversize = produced + 64;

    std::vector<std::vector<float>> dst ((size_t) channels,
                                         std::vector<float> ((size_t) oversize, 7.5f));
    auto dstPtrs = render_tests::rawPtrs (dst);

    renderWarpedChopLinear (m, srcPtrs.data(), (int) src[0].size(), channels,
                            dstPtrs.data(), oversize);

    // Frames [0..produced) match source. Frames [produced..oversize) are zeroed.
    for (int i = 0; i < produced; ++i)
        CHECK (dst[0][(size_t) i] == src[0][(size_t) i]);
    for (int i = produced; i < oversize; ++i)
        CHECK (dst[0][(size_t) i] == 0.0f);
}

static void test_render_warped_marker_lands_correctly()
{
    currentTest = "render_warped_marker_lands_correctly";
    // Single marker pulls source@4800 (originally 0.1s) to local time 0.05s.
    // Verify the output frame at that local time reads from source[4800].
    const double sr = 48000.0;
    const int chopStart = 0;
    const int chopEnd   = 9600;
    const int channels  = 1;

    auto src = render_tests::makeSource (channels, 12000);
    const auto srcPtrs = render_tests::rawConstPtrs (src);

    ChopWarpMarker m1;
    m1.sourceSample     = 4800;
    m1.localTimeSeconds = 0.05;

    WarpMap m;
    m.build (chopStart, chopEnd, { m1 }, sr);
    CHECK (m.numDroppedMarkers() == 0);

    const int frames = warpedChopFrameCount (m);
    std::vector<std::vector<float>> dst ((size_t) channels,
                                         std::vector<float> ((size_t) frames, 0.0f));
    auto dstPtrs = render_tests::rawPtrs (dst);

    renderWarpedChopLinear (m, srcPtrs.data(), (int) src[0].size(), channels,
                            dstPtrs.data(), frames);

    // The output frame at local time 0.05s = frame 2400 should equal source[4800].
    const int markerOutFrame = (int) std::llround (0.05 * sr);
    CHECK (dst[0][(size_t) markerOutFrame] == src[0][4800]);

    // First frame should equal source[chopStart] (= source[0]).
    CHECK (dst[0][0] == src[0][0]);

    // First-segment speed is 2.0, so every output frame in [0, 2400] maps to
    // an even integer source sample → bit-exact.
    CHECK (dst[0][1] == src[0][2]);
    CHECK (dst[0][1200] == src[0][2400]);

    // Second-segment speed is 0.6667 source-samples per output-frame;
    // +3 frames from the marker → +2 source samples.
    CHECK (dst[0][2403] == src[0][4802]);

    // Last frame lands on a fractional source position — verify it's the
    // linear interp of the bracketing samples (≈9598.4).
    const double lastLocalTime = (double) (frames - 1) / sr;
    const double lastSrcPos    = m.sourceSampleAtLocalTime (lastLocalTime);
    const int    i0 = (int) lastSrcPos;
    const float  expected = (float) (src[0][(size_t) i0]
                                     + (src[0][(size_t) (i0 + 1)] - src[0][(size_t) i0])
                                       * (lastSrcPos - (double) i0));
    CHECK_CLOSE ((double) dst[0][(size_t) (frames - 1)], (double) expected, 1e-3);
}

static void test_render_handles_null_inputs_safely()
{
    currentTest = "render_handles_null_inputs_safely";
    WarpMap m;
    m.build (0, 480, {}, 48000.0);

    // Null target → no-op, no crash.
    renderWarpedChopLinear (m, nullptr, 0, 1, nullptr, 0);

    // Null source → target zeroed.
    const int channels = 1;
    const int frames   = warpedChopFrameCount (m);
    std::vector<std::vector<float>> dst ((size_t) channels,
                                         std::vector<float> ((size_t) frames, 4.2f));
    auto dstPtrs = render_tests::rawPtrs (dst);

    renderWarpedChopLinear (m, nullptr, 0, channels, dstPtrs.data(), frames);
    for (int i = 0; i < frames; ++i)
        CHECK (dst[0][(size_t) i] == 0.0f);
}

// --- Stress tests (step 13) -----------------------------------------------

static void test_speed_clamp_upper_boundary_accepted()
{
    currentTest = "speed_clamp_upper_boundary_accepted";
    // Speed exactly at 4.0 should be accepted at the upper boundary.
    // sourceDuration = 4800/48000 = 0.1 s, localDuration = 0.025 s → speed = 4.0
    const double sr = 48000.0;
    ChopWarpMarker m1;
    m1.sourceSample     = 4800;
    m1.localTimeSeconds = 0.025;

    WarpMap m;
    m.build (0, 9600, { m1 }, sr);

    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 1);
}

static void test_many_markers()
{
    currentTest = "many_markers";
    // Place 50 evenly-spaced markers, each at its natural local time so no
    // segment exceeds the speed clamp. Verify all are accepted, ordering
    // holds, and lookups stay sane across the entire range.
    const double sr = 48000.0;
    const int    chopStart = 0;
    const int    chopEnd   = 480000; // 10 s
    const int    nMarkers  = 50;

    std::vector<ChopWarpMarker> markers;
    markers.reserve ((size_t) nMarkers);
    for (int i = 1; i <= nMarkers; ++i)
    {
        ChopWarpMarker m;
        m.sourceSample     = (chopStart * (nMarkers + 1 - i) + chopEnd * i) / (nMarkers + 1);
        m.localTimeSeconds = (double) (m.sourceSample - chopStart) / sr;
        markers.push_back (m);
    }

    WarpMap m;
    m.build (chopStart, chopEnd, markers, sr);
    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == nMarkers);

    // Forward lookup at every marker's localTime returns its sourceSample.
    for (const auto& mk : markers)
    {
        const double s = m.sourceSampleAtLocalTime (mk.localTimeSeconds);
        CHECK_CLOSE (s, (double) mk.sourceSample, 1e-3);
    }

    // Inverse round-trip across the chop.
    for (int i = 0; i <= 1000; ++i)
    {
        const double t = (m.totalLocalDurationSeconds() * i) / 1000.0;
        const double s = m.sourceSampleAtLocalTime (t);
        CHECK_CLOSE (m.localTimeAtSourceSample (s), t, 1e-6);
    }
}

static void test_tiny_chop_does_not_crash()
{
    currentTest = "tiny_chop_does_not_crash";
    // Smallest meaningful chop: exactly 2 samples wide (one source-step).
    WarpMap m;
    m.build (1000, 1002, {}, 48000.0);
    CHECK (m.isIdentity());
    CHECK (m.totalLocalDurationSeconds() > 0.0);

    // Forward at endpoints.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.0), 1000.0, 1e-9);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (m.totalLocalDurationSeconds()), 1002.0, 1e-9);

    // Inverse.
    CHECK_CLOSE (m.localTimeAtSourceSample (1001.0), m.totalLocalDurationSeconds() * 0.5, 1e-9);

    // Tiny chop with a marker proposed inside should be rejected (no room
    // for a marker between sample 1000 and 1002).
    ChopWarpMarker badMarker;
    badMarker.sourceSample = 1001;
    badMarker.localTimeSeconds = m.totalLocalDurationSeconds() * 0.5;
    WarpMap m2;
    m2.build (1000, 1002, { badMarker }, 48000.0);
    // Either accepted with valid speed, or dropped — either way must not crash.
    CHECK (m2.totalLocalDurationSeconds() > 0.0);
}

static void test_long_chop()
{
    currentTest = "long_chop";
    // 60-second chop at 48 kHz = 2.88M samples.
    const double sr = 48000.0;
    const int chopStart = 0;
    const int chopEnd   = 2880000;

    ChopWarpMarker m1;
    m1.sourceSample     = 1440000;
    m1.localTimeSeconds = 25.0; // pulled 5 s earlier than natural 30 s

    WarpMap m;
    m.build (chopStart, chopEnd, { m1 }, sr);
    CHECK (m.numDroppedMarkers() == 0);

    // Forward at marker.
    CHECK_CLOSE (m.sourceSampleAtLocalTime (25.0), 1440000.0, 1.0);

    // Total duration unchanged (chop length pinned).
    CHECK_CLOSE (m.totalLocalDurationSeconds(), 60.0, 1e-9);

    // First-segment speed = 1440000/(48000*25) = 1.2; below clamp ceiling.
    // Second-segment speed = 1440000/(48000*35) ≈ 0.857; above clamp floor.
    // Should both be accepted.
    CHECK (m.numInternalMarkers() == 1);
}

static void test_marker_at_natural_position_is_lossless()
{
    currentTest = "marker_at_natural_position_is_lossless";
    // A marker placed exactly at its natural local time should produce a
    // bit-exact identity render across the whole chop.
    const double sr = 48000.0;
    const int chopStart = 0;
    const int chopEnd   = 9600;
    const int channels  = 1;

    auto src = render_tests::makeSource (channels, 12000);
    const auto srcPtrs = render_tests::rawConstPtrs (src);

    ChopWarpMarker m1;
    m1.sourceSample     = 4800;
    m1.localTimeSeconds = (4800 - chopStart) / sr;

    WarpMap m;
    m.build (chopStart, chopEnd, { m1 }, sr);
    CHECK (m.numDroppedMarkers() == 0);
    CHECK (m.numInternalMarkers() == 1);

    const int frames = warpedChopFrameCount (m);
    std::vector<std::vector<float>> dst ((size_t) channels,
                                         std::vector<float> ((size_t) frames, 0.0f));
    auto dstPtrs = render_tests::rawPtrs (dst);

    renderWarpedChopLinear (m, srcPtrs.data(), (int) src[0].size(), channels,
                            dstPtrs.data(), frames);

    // Even with a marker, a natural-position marker should produce the same
    // output as identity render (positions land on integer source samples).
    for (int i = 0; i < frames; ++i)
        CHECK (dst[0][(size_t) i] == src[0][(size_t) (chopStart + i)]);
}

static void test_repeated_builds_do_not_leak_state()
{
    currentTest = "repeated_builds_do_not_leak_state";
    // Simulates rapid marker edits — calling build() many times in succession
    // must always converge to the same state for identical inputs.
    const double sr = 48000.0;
    ChopWarpMarker m1;
    m1.sourceSample     = 2400;
    m1.localTimeSeconds = 0.04;

    WarpMap m;
    for (int i = 0; i < 1000; ++i)
        m.build (0, 9600, { m1 }, sr);

    CHECK (m.numInternalMarkers() == 1);
    CHECK (m.numDroppedMarkers() == 0);
    CHECK_CLOSE (m.sourceSampleAtLocalTime (0.04), 2400.0, 1e-6);
}

// --- Driver ----------------------------------------------------------------

int main()
{
    test_identity_no_markers();
    test_endpoint_clamping();
    test_invalid_inputs();
    test_single_marker_identity_position();
    test_single_marker_warped();
    test_multiple_markers();
    test_speed_clamp_rejects_extreme_stretch();
    test_speed_clamp_rejects_extreme_compression();
    test_marker_outside_chop_rejected();
    test_unsorted_markers_are_sorted();
    test_non_monotonic_local_time_rejected();
    test_round_trip_random();
    test_speed_clamp_boundary_accepted();

    test_render_identity_is_bit_exact();
    test_render_target_zeroed_when_oversized();
    test_render_warped_marker_lands_correctly();
    test_render_handles_null_inputs_safely();

    test_speed_clamp_upper_boundary_accepted();
    test_many_markers();
    test_tiny_chop_does_not_crash();
    test_long_chop();
    test_marker_at_natural_position_is_lossless();
    test_repeated_builds_do_not_leak_state();

    std::printf ("WarpMap tests: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
