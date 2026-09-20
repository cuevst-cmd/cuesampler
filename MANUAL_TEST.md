# Stem Separation — Manual Test Checklist

End-to-end manual verification for the HTDemucs stem-mute feature. Run after any
change to the separation / mute / cache-refresh paths. Assumes the single model is
present at `assets/htdemucs/htdemucs.onnx`; on Windows, fetch and verify it with
`download-htdemucs-model.ps1`.
Load the VST3 or AU in a DAW (or AudioPluginHost); watch the log for lines
prefixed `StemSeparator:`.

## Chop playback modes
- [ ] Both **GATE** and **ONE SHOT** choices stay visible; exactly the active mode is highlighted.
- [ ] Click/hold a waveform chop in **GATE**: it loops until mouse-up, then releases.
      In **ONE SHOT**, a quick click plays to the end once after mouse-up; holding
      longer than the chop must not restart it. Repeat with an unassigned chop.
- [ ] Re-click a waveform chop: it retriggers from its cue. Switching modes mid-click
      affects the next trigger; the current voice retains the mode it started in.
- [ ] Closing the editor releases a held mouse Gate. Stop cancels a mouse One Shot.
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
- [ ] Select a chop and click its floating **ADSR** button. The compact callout opens
      with Attack, Decay, Sustain, and Release knobs; use the separate **DRAG AUDIO** handle for
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

## MIDI mapping is visible (note names, lit keys, unreachable chops)
- [ ] Load a sample. Every chop header wide enough to show it carries the key that
      fires it, on the right of its number. Narrow the zoom until headers are under
      ~52 px — the note name drops, the number stays.
- [ ] Every header name matches the key that actually triggers that chop. Check at
      several **OCT +/-** positions; names and lit keys update immediately.
- [ ] The on-screen keyboard shades every key that triggers a chop. Three states must
      be tellable apart at a glance: unmapped, mapped-but-silent, and sounding.
      Verify in both light and dark themes.
- [ ] Enter manual mode and pin a chop to a key that a positional chop already
      resolves to (e.g. capture on C2 with the root at C2). The displaced chop's
      header shows a dimmed `-`, and the status line under CHOP/BARS reads
      `n CHOPS UNREACHABLE`. Before this change that chop was silently dead.
- [ ] The unreachable chop is still selectable, previewable and exportable by mouse.
- [ ] Clear the collision (delete or reassign the pinned chop). The dash disappears,
      the status line returns to `ROOT <note> | KEY SHOWN ON EACH CHOP`.
- [ ] A chop carrying warp markers shows both the wave glyph and its note name without
      the two overlapping; below ~72 px the glyph gives way.

## BARS is a direct selector
- [ ] The BARS control shows four segments — 1 / 2 / 4 / 8 — with the active one lit.
      One click reaches any value; there is no cycling.
- [ ] Click the already-lit segment. Nothing rebuilds, the chop list is untouched, and
      no undo step is consumed (press UNDO — it must step past this click, not onto it).
- [ ] Save a project on 4 BARS, reopen: the 4 segment comes back lit.
- [ ] Undo a bars change — the segments follow the restored value.

## Chop edits follow the audio across a rebuild
- [ ] At 1 BAR, give chop 7 a distinctive envelope, gain, cue point and a warp marker.
      Switch to 2 BARS. Those edits must land on the chop covering the **same audio**
      (bars 7-8), not on the chop that happens to be 7th in the list.
- [ ] Switch 1 -> 2 -> 4 -> 8 -> 1 and back. No chop ever ends up sharing a key with
      another (watch the status line for UNREACHABLE, which must stay at zero).
- [ ] Pin two adjacent chops to different pads at 2 BARS, then switch to 1 BAR. Each
      pad stays on one chop; the split twins fall back to the positional map rather
      than going permanently unassigned.
- [ ] Set a cue part-way into a chop, then nudge TEMPO trim / grid offset. The cue
      keeps pointing at the same moment of audio rather than the same offset.
- [ ] Shrink a chop until the old cue would fall outside it — it falls back to the
      auto-detected cue and playback still starts cleanly.
- [ ] Nudge the grid hard (a large offset). Edits re-home sensibly; nothing is
      duplicated onto two chops.

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
      the panel shows LOADING STEMS then READY (embedded audio is decoded without another
      separation pass), and the first audible mix has vocals muted as saved.

## Reset / second sample
- [ ] Load a different sample while the first is still SEPARATING → progress resets, the old
      job is abandoned, and none of its audio or stems reappear in the new sample.
- [ ] After loading a fresh sample, all three mutes reset to OFF. Cached stems may load;
      otherwise the panel offers SEPARATE for a user-initiated pass.

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

## DAW recall regression (state version 7)

New saves embed 32-bit floating-point source audio and separated stems. Project
files will be larger. A completed separation no longer depends on the optional
disk cache or the model being installed when reopening. Older projects remain
readable; successfully recovered cache-only stems are embedded on the next save.
Audio already quantized/clipped by an older save cannot be reconstructed exactly.

- Separate a sample, mute DRUMS and VOCALS, edit cue/gain/ADSR and warp markers,
  choose a BARS segment, set the octave, and select a chop. Save the DAW project.
- Close the project, temporarily move its original sample and stem-cache folder
  aside, then reopen. Check the same sound, mute buttons, both chop layers, MIDI
  mapping, selected chop, waveform view, and playhead. Restore the moved files.
- During restore, confirm that the unmuted source never briefly plays. Save
  immediately during loading and reopen that save; its audio must remain intact.
- Unmute everything after reopening: the original source must return without
  doubled stem subtraction. Compare an offline render before/after reopen.
- With the editor open, change BARS, undo, and restore a different saved project.
  Confirm the lit segment always agrees with the actual chop length.
- Place a warp marker late in a 2-bar chop, then select 1 bar. Confirm the marker
  in the later child retains its timing and remains active.
- While stems/cache are loading, restore an empty project or a different sample.
  The old sample and mutes must not reappear after the background job finishes.

Automated regression checks (plugin installation is disabled):

```sh
cmake -S . -B build -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF -DCUE_BUILD_STEM_CACHE_TEST=ON -DCUE_BUILD_STATE_TEST=ON
cmake --build build --target test_stem_cache test_project_restore
ctest --test-dir build --output-on-failure
```

## Audio extraction regression

Exports use 32-bit floating-point WAV at the host sample rate and snapshot the
chop, cue, pitch, warp, tempo/HALF-TIME, gain, envelope, reverse and stem mutes at
the export gesture. Rendering and Save As copies run off the UI/audio threads.
The envelope follows full-velocity one-shot playback; a held MIDI gate/note-off
performance is not recorded into the file. Prepared audio preserves headroom
instead of independently normalising each chop.

- [ ] Drag the DRAG AUDIO handle with pitch, warp, SYNC and HALF-TIME individually,
      then combined. Place each WAV on the grid and compare its onset, duration,
      pitch and final transient to the loop; repeat at 44.1/48/96 kHz and with a
      source at a different rate. Check chops at the start/end of the source.
- [ ] Export just after moving a warp marker, changing a cue or toggling a stem
      mute. The export must reflect that gesture's settings, including a mute
      whose background playback remix has not finished yet.
- [ ] Test reverse, nonzero cue, source peaks above 0 dBFS, global/chop gains,
      attack/sustain and a release longer than the entire chop. Short attacking
      chops must remain audible and the file must not clip/normalise float peaks.
- [ ] Cold-render a long processed chop. The editor should remain responsive and
      display Preparing export. Keep dragging to start the native file drag once
      ready. Release early, then drag the handle again: the prepared result is reused
      if settings are unchanged. Change settings before retrying: it must re-render.
- [ ] Use ADSR > Save WAV, cancel it, and test a destination that cannot be written.
      Failed saves report an error and preserve the rendered file and any existing
      destination file. Close the editor while rendering; no crash or late dialog.
- [ ] Drag to a DAW that references files in place. Wait more than 60 seconds,
      close the plugin, save/reopen the DAW session and verify the clip still plays.
      Successful drag files stay in `CueSampler/Exports` under JUCE's user application
      data directory (macOS: `~/Library/CueSampler/Exports`). They are not automatically
      deleted; don't manually remove files still referenced by projects. Use the
      DAW's collect/copy-media feature before moving a project to another computer.

The `test_project_restore` suite also reads actual exported WAVs and checks
processing-delay compensation, loop lengths, pitch, late-loop audio, sample-rate
conversion, float peaks, cue/reverse, stem intent, long-release short chops,
background snapshots/cache lifetime, and stem-resampling alignment. Fresh stem
cache keys include the corrected resampler revision; old saved projects preserve
their embedded stems until explicitly separated again.

## Stem quality and readiness (10% overlap / sinc resampling)

New separations use 10% overlap and a band-limited, zero-phase sample-rate
converter. Overlap adds roughly 11% more inference windows on long files versus
zero overlap; short-file counts depend on length. Processing stops as soon as the
last window covers the source. Optional disk-cache writes run on a separate
worker after READY; the portable project payload is prepared before READY.
Existing saved stems retain their original sound until separated again.

- [ ] Separate music with drums and vocals spanning the 7.02-second window stride.
      Listen across boundaries for clicks, pumping and bleed, comparing the same
      source with `CUE_STEM_OVERLAP=0` and the default (unset). Compare identical
      regions and levels; overlap does not guarantee better isolation on every song.
- [ ] Try 8, 22.05, 44.1, 48 and 96 kHz sources. Check attacks remain aligned with
      the original and that exported stem-muted loops match playback.
- [ ] At READY, immediately change mute buttons, export a loop and save the DAW
      project. Reopen with the original/cache unavailable and verify the exact mix,
      chop settings and audio are retained. Repeat on a slow or unwritable cache.
- [ ] Benchmark representative full songs separately from first-use model loading,
      with several plugin instances playing at 64/128-sample host buffers. Check
      playback stability, elapsed time and memory on macOS and Windows.

Automated coverage: `test_stem_cache` checks setting-sensitive keys and complete,
nonredundant window coverage. `test_project_restore` checks resampler passband,
alias rejection, timing, short buffers and exact equal-rate copies. Run
`test_project_restore <scratch-parent> --separate` with the model installed to
exercise real inference, blocked cache writing, independent mute remixing and
bit-exact portable save/restore before the cache exists.

## Sample tempo 2x correction

- [ ] Load a sample detected at 60 BPM and click **2x** next to the BPM readout.
      The button lights up and BPM reads 120; click again to return to 60.
- [ ] With one-bar automatic chops, boundaries change from every four seconds to
      every two seconds. Grid lines and warp snap divisions follow the corrected
      BPM. Manually placed chop boundaries stay in place.
- [ ] In a 120 BPM DAW with SYNC enabled, the corrected 120 BPM sample plays at
      its native tempo. Export a one-bar chop and confirm a two-second file.
      Without SYNC, correcting the detected BPM does not change playback speed.
- [ ] Apply a +0.5 BPM trim: 2x uses 120.5 BPM, retaining the fine trim in BPM units.
      Confirm manual BPM entry and shift-resize use the corrected tempo too.
- [ ] Undo restores the prior BPM, chop layout and sync ratio. Save/reopen a
      project with 2x enabled and verify the lit button, grid and exports.
- [ ] Older projects open with 2x off; loading a different sample resets it.
      The button is disabled while analysis is running or no sample is loaded.

`test_project_restore` exercises the real button, tempo/grid/sync/export math,
Undo, fine trim, manual chops, saved-state recall and older-state defaults.

## Favorite keyboard colors

- [ ] Double-click a chop to favorite it: both the chop and its mapped visual
      keyboard key turn pink. Check white and black keys.
- [ ] Select or play a favorite: its key remains pink, with a stronger highlight.
      Unfavorite it and confirm the usual mapped/selected color returns.
- [ ] Undo, change OCT, switch chop layers and reopen the editor/project. Pink
      follows the current resolved MIDI mapping and saved favorite state; an
      unreachable chop must not tint a key owned by a different chop.

## Favorites performance view

- [ ] Favorite chops out of sample order (for example 3, 1, 6, 4). Click
      **FAVORITES** above the waveform: only those chops appear, side by side,
      labeled C2, C#2, D2, D#2 in the order they were favorited. Pink keyboard
      keys and incoming MIDI must agree with the tile labels.
- [ ] Click/hold a tile in GATE mode, then release it. Try ONE SHOT and hardware
      MIDI too. Cue, gain, pitch, reverse, warp and stem settings still belong
      to the original chops. This view does not concatenate or rewrite audio.
- [ ] Double-click a tile to unfavorite it. Remaining assignments close the gap.
      Undo restores its position. Re-favorite a removed chop from the full view:
      it goes to the end. Remapping while holding a key must not leave a stuck note.
- [ ] Toggle Favorites off. Original explicit MIDI assignments, unassigned chops,
      octave offset, full waveform zoom/scroll and source boundaries return.
      In Favorites, octave and waveform-editing controls are disabled; favorite
      tiles wrap into rows and fit within the waveform panel without scrolling.
- [ ] Save/reopen with Favorites on, including favorites from a manual chop layout.
      The current view, favorite order and temporary MIDI map return. Switching
      it off after recall still restores the original mappings.
- [ ] Remove every favorite: show the empty-state instructions and no mapped keys.
      Load another sample: Favorites returns off. Old projects without favorite
      chronology use sample order initially; new favorite actions record order.
- [ ] With 8, 18 and 24 favorites, verify every tile fits on screen in rows.
      Resize the window: labels and waveforms stay inside their tiles. Play and
      remove tiles in each row; gaps and empty cells must not trigger a chop.
      The first 92 favorites map from MIDI 36 (C2) through 127 (G9); additional
      tiles explicitly display NO MIDI. Favorites are taken from the active chop
      layout; switch the manual/automatic layout in the full view.

`test_project_restore` checks chronology, the real Favorites button, C2 MIDI
triggering, original-pin suppression/restoration, original audio/settings,
held-voice reset on remap, removal/re-add, Undo, portable recall and old states.

## Explicit chop export handle
- [ ] Selecting a chop shows separate **ADSR** and **DRAG AUDIO** controls. They
      remain available in manual and warp modes without obscuring the +/- buttons.
- [ ] Hold the waveform for more than two seconds: this only previews audio;
      no export countdown or export gesture is armed.
- [ ] Press DRAG AUDIO: show **PREPARING...** while the worker renders. Moving
      the mouse starts a native audio-file drag as soon as rendering is ready.
- [ ] Release before it is ready. No delayed drag or Save dialog should appear;
      a checkmark and **AUDIO READY** message invite the next drag.
- [ ] A simple click on DRAG AUDIO prepares the audio without opening ADSR or
      toggling a favorite. Repeating the click reuses the prepared file.
- [ ] Change pitch/warp/stems after preparing: the next export uses fresh settings.
      Select another chop while rendering: its next request supersedes the old UI result.
- [ ] ADSR opens its envelope menu; **SAVE WAV...** opens the file-save workflow.
- [ ] Test actual drops in a DAW on macOS and Windows, including a cold render,
      early release/retry, rejected drop and repeat drag of unchanged audio.

## Knob defaults
- [ ] Change each main knob, then Option-click (Mac) / Alt-click (Windows):
      CUE, both GAIN/PITCH controls, TEMPO trim, ZOOM and SCROLL return to zero.
- [ ] ADSR resets to Attack 0 ms, Decay 0 ms, Sustain 100%, Release 5 ms.
      Confirm the selected chop's stored settings match the knobs.
- [ ] After dragging a knob, Option-click and move the mouse before release:
      the reset value stays put. Normal dragging still works on the next gesture.
- [ ] Knob tooltips name the platform's reset modifier. Double-click also resets.
