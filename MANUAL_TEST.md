# Stem Separation — Manual Test Checklist

End-to-end manual verification for the HTDemucs-FT stem-mute feature. Run after any
change to the separation / mute / cache-refresh paths. Assumes the three models are
present in `assets/htdemucs_ft/` (drums.onnx, bass.onnx, vocals.onnx) — produce them
with `export_htdemucs.py` or download from `StemSplitio/htdemucs-ft-onnx` on Hugging
Face. Load the VST3 or AU in a DAW (or AudioPluginHost); watch the log for lines
prefixed `StemSeparator:`.

## Happy path
- [ ] Load a short song (≤ a few minutes). The STEMS panel shows **SEPARATING n%** with
      the three buttons disabled/dimmed; progress climbs.
- [ ] Log shows `StemSeparator: ... ready=YES` at startup and (macOS) either
      `CoreML execution provider enabled` or `CoreML EP unavailable, using CPU`.
- [ ] When done, status shows **READY** and BASS / DRUMS / VOCALS become enabled.
- [ ] Toggle **VOCALS** → the keycap lights; vocals drop out of playback within a moment.
      Toggle off → vocals return. Repeat for **DRUMS** and **BASS** (audible each time).
- [ ] With all three OFF, playback sounds identical to the original (bit-for-bit).
- [ ] Mute two stems at once (e.g. DRUMS + VOCALS) → only bass + "other" remain.

## Interaction with existing features (no regressions)
- [ ] With a stem muted, trigger chops via the on-screen keyboard / MIDI — chops play the
      muted mix, no silence, no stale (pre-mute) audio.
- [ ] Add warp markers to a chop, then toggle a mute — the warped chop re-bakes against the
      muted audio (no stale warp bake); playback stays warped and correct.
- [ ] Toggle HALF-TIME and/or SYNC TO DAW while a stem is muted — speed/sync still apply to
      the muted mix; no glitches.
- [ ] Zoom / scroll the (now 330 px) waveform, select chops, drag chop edges — all still work.

## Persistence
- [ ] Mute VOCALS, save the project, close and reopen. The VOCALS button comes back muted,
      the panel shows SEPARATING then READY (stems re-separate; audio is NOT serialized),
      and once READY the vocals are muted as saved.

## Reset / second sample
- [ ] Load a different sample while the first is still SEPARATING → progress resets, the old
      job is abandoned (log shows the new generation start), the new sample separates.
- [ ] After loading a fresh sample, all three mutes reset to OFF and the panel re-runs
      SEPARATING → READY.

## Edge cases
- [ ] No sample loaded → panel is neutral (title only / blank status), buttons disabled.
- [ ] Models missing (empty `assets/htdemucs_ft/`) → status **NO MODEL**, buttons disabled,
      original plays normally, log: `separation unavailable (models not ready)`. No crash.
- [ ] Load a very long sample (> 10 min) → status **SAMPLE TOO LONG**, buttons disabled,
      original plays, log: `sample too long (... ) — skipping separation`. (Constant:
      `kMaxStemSeparationSeconds` in PluginProcessor.h.)

## Memory
- [ ] Load several different samples in a row while watching the process's memory. It should
      plateau, not climb monotonically — each load drops the previous `StemSet` (pristine
      source + 3 stem buffers) once the new sample's separation publishes.

## Automated companion
The offline `tools/test_stem_separator.cpp` (build with `-DCUE_BUILD_STEM_TEST=ON`,
target `test_stem_separator`) asserts stem shapes, the subtraction reconstruction, per-stem
energy/distinctness, and the mute-mix model (no-mute == original; vocals-muted ==
original − vocals). Run it on a short stereo WAV with the models present.
