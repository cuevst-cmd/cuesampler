# Stem Separation — Manual Test Checklist

End-to-end manual verification for the HTDemucs stem-mute feature. Run after any
change to the separation / mute / cache-refresh paths. Assumes the single model is
present at `assets/htdemucs/htdemucs.onnx`; on Windows, fetch and verify it with
`download-htdemucs-model.ps1`.
Load the VST3 or AU in a DAW (or AudioPluginHost); watch the log for lines
prefixed `StemSeparator:`.

## Chop playback modes
- [ ] In **GATE** mode, hold a mapped MIDI note past the chop end. The chop loops from its
      cue point until note-off, then releases cleanly without a click.
- [ ] In **GATE** mode, release before the chop end. Playback stops immediately with the
      short de-click fade.
- [ ] Switch to **ONE SHOT** and tap/release a mapped MIDI note. The chop ignores note-off,
      plays from its cue point to its end exactly once, and does not loop.
- [ ] Trigger another chop before the One-Shot finishes. The new chop chokes the old voice
      and begins immediately; the two chops never overlap.
- [ ] Check normal, reversed, pitched, warped, HALF-TIME, and SYNC playback in both modes.
- [ ] Save a project in **ONE SHOT**, close/reopen it, and confirm the button and behavior
      restore as One-Shot. Older projects should open in **GATE** mode.

## Per-chop ADSR and export
- [ ] Select a chop and click its floating **ADSR / EXPORT** pill. The compact callout opens
      with Attack, Decay, Sustain, and Release knobs; dragging the pill still starts a direct
      file drag into the DAW.
- [ ] Give two chops clearly different envelopes and retrigger each from MIDI. Attack/decay/
      sustain remain independent per chop; in GATE mode Release begins on note-off.
- [ ] In ONE SHOT mode, confirm Release fades into the chop boundary without shortening or
      looping the chop. Exercise normal, pitched, warped, reversed, HALF-TIME, and SYNC playback.
- [ ] Use **EXPORT CHOP** inside the callout and confirm the saved WAV has the selected chop's
      ADSR baked in. Save/reopen the project and confirm all four values restore per chop.
- [ ] Enter manual-chop mode, complete and select a chop, then open the same ADSR/export callout.
      Changing the selection while a callout is open must not redirect its knobs to another chop.

## Manual chopping
- [ ] Press **CHOP MANUALLY** after automatic chops exist. The waveform immediately becomes
      a clean slate and shows the double-click start instruction.
- [ ] Double-click a start point, hold a hardware or on-screen MIDI key/pad, and release it
      later in the audio. One numbered chop appears from the start point to the exact release
      position and retriggers only from the MIDI note used during capture.
- [ ] Place another start marker, then double-click its end without touching MIDI. A numbered,
      sequentially MIDI-mapped chop appears and MIDI capture is cleanly cancelled.
- [ ] Create several chops out of timeline order. Labels remain numbered in waveform order,
      while each MIDI-captured chop retains its exact key/pad assignment.
- [ ] Hover either boundary of any completed manual chop. The cursor changes to horizontal
      resize; drag the start and end markers independently and confirm the chop bounds update,
      the label follows waveform order, and its MIDI key/pad assignment is unchanged.
- [ ] With overlapping or adjacent chops, click a chop to select it, then resize its shared or
      nearby boundary. The selected chop's marker wins when two edges occupy the same position.
- [ ] Reassign the same MIDI note to a newer chop. The newer chop owns the pad; the older chop
      remains selectable and exportable by mouse.
- [ ] Start manual mode while tempo analysis is still running. Its late result must not refill
      the cleared chop list. Toggle manual mode off, save/reopen, and confirm completed chop
      bounds and MIDI assignments persist.
- [ ] While holding a pad during capture, confirm the live end guide follows playback and audio
      remains clean throughout the hold and click-free on release. Exercise short taps, long holds,
      release at sample end, zoom, scroll, and several host buffer sizes (especially 64/128 samples).
- [ ] Enable host sync, HALF TIME, and a non-zero pitch setting before capturing. The in-progress
      manual audition still plays at the sample's native pitch/speed without crackles, and the
      completed chop resumes the configured playback processing when retriggered.

## Manual chop: ghost line, marker editing, delete, default key
- [ ] In manual mode, an orange ghost line with an arrowhead follows the mouse across the
      waveform. It disappears while dragging a marker or a chop edge, and returns on release.
      Outside manual mode the guide is still neon red (purple in warp mode).
- [ ] Place a start marker, then hover it: cursor becomes horizontal resize, the line thickens
      and shows a soft halo. Drag it — the marker moves and a subsequent pad hold auditions
      from the new position, not the original one.
- [ ] While holding a pad mid-capture, the marker must NOT be draggable or removable.
- [ ] Double-click the pending marker to remove it. Double-clicking anywhere else still sets
      the chop end.
- [ ] Select a completed chop in manual mode. A red **×** badge appears immediately right of
      the ADSR/EXPORT pill. Click it and the chop is removed; undo restores it.
- [ ] Press the × but release the mouse away from it — nothing is deleted.
- [ ] Scroll a selected chop to the far left and far right edges of the view. The × never
      overlaps the +/- zoom buttons and never leaves the display.
- [ ] Outside manual mode no × appears, and the ADSR/EXPORT pill sits exactly where it did
      before (position must be unchanged).
- [ ] Complete a manual chop by double-clicking the end (no pad held). It is assigned **C3**.
      The next such chop takes C#3, then D3. Delete the C#3 chop and create another — it
      reuses C#3 rather than continuing upward.
- [ ] Chops captured by holding a pad keep that exact pad; the auto-assigned ones skip over
      any note already taken.
- [ ] Load an older project saved before this change. Its chops still map from C2 as before.

## WARP discoverability
- [ ] Press **WARP**. No full-screen guide appears. A purple hint bar sits at the top of the
      waveform reading "CLICK INSIDE A CHOP TO DROP A MARKER ...".
- [ ] Start dragging a marker. The hint changes to the dragging text and says it is snapping.
      Hold **Shift** mid-drag without moving the mouse — the hint updates to "DRAGGING FREELY".
- [ ] Press the **?** button while in WARP mode. The warp guide opens (not the general guide).
      Press it again to close. Escape also closes it.
- [ ] Press **?** outside WARP mode. The normal quick-start guide opens, as before.
- [ ] Leave WARP mode while the warp guide is open. The guide closes with the mode.
- [ ] Add warp markers to a chop, then leave WARP mode. A small purple wave glyph appears on
      that chop's header tab. Chops with no markers show no glyph.
- [ ] Clear the markers with CLEAR ALL — the glyph disappears.
- [ ] Zoom until chops are very narrow. The glyph hides rather than colliding with the chop
      number (headers under ~34 px wide).

## Two chop layers (automatic <-> manual)
- [ ] With automatic chops on screen, press **CHOP MANUALLY**. The waveform clears to a blank
      slate and there is no confirmation prompt (nothing is being destroyed).
- [ ] Build several manual chops, then press the button again. All the original automatic
      chops come back exactly as they were.
- [ ] Press it a third time. The manual chops return exactly as you left them, including their
      MIDI assignments, ADSR values, cue points, and which one was selected.
- [ ] Toggle back and forth several times. Neither set degrades or leaks chops into the other.
- [ ] Save the project while on the manual layer, close and reopen it. It reopens on the
      manual layer with the button lit, and toggling still brings the automatic set back.
- [ ] Repeat saving while on the automatic layer — the manual set must survive too.
- [ ] Close the plugin window while in manual mode and reopen it. The chop list is unchanged
      and the window comes back up in manual mode.
- [ ] Create a chop on each layer and confirm their numbers never collide (ids are shared
      across layers, so exported/cached audio must never come back as the wrong chop).
- [ ] Undo is cleared by a layer switch: switch layers and confirm undo does not paste the
      previous layer's chops over the current set.
- [ ] Enter manual mode while warp mode is on. Warp switches off, as before.

## Cue point follows the start marker
- [ ] Set a chop's cue part-way in, then drag the chop's **start** edge earlier. The cue keeps
      its position relative to the start rather than staying glued to the original audio.
- [ ] Drag the same start edge later. The cue still travels with it and does not collapse
      onto the chop start.
- [ ] Drag only the **end** edge. The cue must not move at all.
- [ ] Shrink a chop until it is shorter than its cue offset. The cue clamps to just inside the
      new end instead of pointing past it, and playback still starts cleanly.
- [ ] Repeat all of the above outside manual mode — behaviour is intentionally identical now.

## Happy path
- [ ] Load a short song (≤ a few minutes). The STEMS panel shows **SEPARATING n%** with
      the three buttons disabled/dimmed; progress climbs.
- [ ] Log shows `StemSeparator: ... ready=YES` after the first separation request.
      Windows should report DirectML or a clean CPU fallback; macOS uses CPU unless
      `CUE_ENABLE_COREML=1` is explicitly set.
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
- [ ] Model missing (empty `assets/htdemucs/`) → status **NO MODEL**, buttons disabled,
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
original − vocals). Run it on a short stereo WAV with the model present.
