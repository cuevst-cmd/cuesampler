# Stem Separation — Manual Test Checklist

End-to-end manual verification for the HTDemucs stem-mute feature. Run after any
change to the separation / mute / cache-refresh paths. Assumes the single model is
present at `assets/htdemucs/htdemucs.onnx`; on Windows, fetch and verify it with
`download-htdemucs-model.ps1`.
Load the VST3 or AU in a DAW (or AudioPluginHost); watch the log for lines
prefixed `StemSeparator:`.

## Older Mac stem performance

- [ ] Generate `htdemucs-compact.onnx` with `tools/optimize_stem_model.py` before
      configuring/building. Verify CMake selects it and the bundle contains that
      file as `Resources/htdemucs/htdemucs.onnx` (same SHA-256).
- [ ] On both an Intel Mac and an M1/M2, separate short and full-length music
      during active DAW playback at a low buffer size. Record chip, sample length,
      first-use/second-use wall time, CPU time and peak RSS. Confirm responsive
      controls and no playback dropouts.
- [ ] Compare original/compact float WAV stems with the offline harness. Listen
      across segment boundaries. Reload a cached sample and reopen its saved
      project; confirm identical mute behavior and no new separation/model load.
- [ ] Repeat with two plugin instances. The four-thread budget is per instance;
      record aggregate host CPU and memory, including model load.

## C4 Window / Favorites performance sets

These checks describe the current mapping for new imports. The older C2 and
channel-bank checks below remain relevant to saved-map compatibility fixtures.
The user performs the FL Studio portion; automated checks do not control the DAW.

- [ ] Load a new sample with at least 220 chops. At FL's default C4 typing octave,
      the first chop plays immediately. The active map is WINDOW, with chops 1-56
      assigned C4-G8. Every source chop stays visible and mouse-playable.
- [ ] Cyan row outlines mark the assigned pads; short bottom ticks mark the notes
      reachable in the current typing octave. Favorite, selection and sounding
      indicators remain readable in both themes.
- [ ] Drag the Window handle in the pad header. The cyan range previews whole
      chops, without audition or MIDI remapping. Release to commit. One Undo
      restores the previous assignments. The final range reaches the last chop.
- [ ] Window / Favorites selector text is larger and readable in both themes;
      DRAG MIDI clearly identifies the range handle. Labels and dropdowns fit at
      minimum editor size, with Micro-Chops immediately left of MIDI.
- [ ] Exercise Micro-Chops OFF / 1/2 / 1/4 / 1/8 / 1/16 BAR. At 120 BPM in 4/4 these
      produce 1000 / 500 / 250 / 125 ms chops. Pads stay comfortably sized; use vertical
      wheel / trackpad scrolling and the scrollbar to reach the final chop.
      Scrolling does not change MIDI assignments or release a held Gate pad.
      OFF returns to the previous whole-bar choice; long sets continue to scroll.
      Clicking that BARS button also exits Micro.
- [ ] Micro changes reset Window to chop 1 and support one-step Undo and save/reopen.
      MIDI Lock and manual chopping prevent changes. Shift-resize still produces
      the correct tempo, and new sample imports retain the selected fraction.
- [ ] Reopen a Micro-Chops project saved with beat fractions. Its BEAT* value, chop
      boundaries and MIDI assignments stay intact, including after ordinary
      tempo/offset edits. Explicitly choose a BAR fraction, even the same one,
      to adopt bar fractions; one Undo restores the previous beat grid. Original
      projects without a saved unit use bars. New imports also use bar fractions
      while retaining the chosen denominator.
- [ ] Switch repeatedly between 1 / 2 / 4 / 8 bars. Window maps consecutive pads
      from C4 with no holes or every-eighth-pad pattern. A moved Window resets to
      chop 1, covering the first 56 chops or all available chops when fewer exist.
      One Undo restores the old bars and exact map; save/reopen retains the result.
- [ ] Repeat bar changes while Favorites is active. Favorites retains its
      cue-following slots and reserved gaps; returning to Window shows a consecutive
      range. MIDI Lock blocks bar changes in either mode.
- [ ] Favorite chops 1 and 200 in that order. Open Favorites and choose USE FOR
      MIDI: C4 plays chop 1 and C#4 plays chop 200. Record the pattern in FL.
- [ ] Return to the waveform and audition outside pads. The recorded Favorites
      pattern keeps the same sounds. Changing the visible view never changes
      the MIDI source; the selector in the pad header shows the active source.
- [ ] Remove chop 1 from Favorites: C4 becomes empty and chop 200 stays on C#4.
      Add another chop: it takes D4. Re-add chop 1: it returns to C4.
- [ ] Add more than 56 favorites. Excess favorites remain mouse-playable and
      show NO MIDI. No implicit MIDI-channel bank or reassignment occurs.
- [ ] Use SLOTS... > Rebuild slots only when intentionally rebuilding a MIDI
      layout. Gaps are reclaimed in addition order. Undo restores all old slots.
- [ ] Enable MIDI LOCK after recording. The mode selector, Window handle, layer
      switch, regrid controls and slot rebuild cannot change assignments. Sound
      edits and direct audition still work. Unlocking alone never remaps notes.
- [ ] Insert or move unassigned chops: existing committed notes keep their chop
      bindings. If the assigned pads cease to be consecutive, the header reports
      fixed assignments and the outlines follow the actual owners.
- [ ] Save/reopen with distinct automatic/manual layouts and MIDI Lock enabled.
      Restore both maps, reservations, active layer and lock. Older projects retain
      their saved root, explicit pins, channel banks and saved Favorites mapping.

## Chronological chop pads
- [ ] The piano keyboard is replaced by numbered pads in source order, left to right,
      then top to bottom. Pads show chop numbers only, including manually assigned,
      unassigned, overlapping, and late-song chops.
- [ ] Exercise short and long songs (16, 64, 220, and 512 chops), both themes, and the
      minimum window size. Pads stay at least 48 px wide and 40 px high, with at
      most three visible rows. Scroll vertically with the wheel, trackpad or
      scrollbar to reach later rows, for both whole-bar and Micro chops. The pad
      window stops growing at 120 px; adding more chops does not shrink the waveform.
- [ ] Scroll to and click the first, middle, and final pads. Each selects the matching
      waveform chop and updates its controls. MIDI assignments remain unchanged.
      Fractional trackpad motion accumulates into whole-row movement; the view
      never exposes a partial fourth row and the scrollbar follows the row offset.
- [ ] A pad follows **GATE** / **ONE SHOT** exactly like a waveform audition. Releasing
      outside the grid, hiding the editor, or closing it cannot leave a held Gate running.
      Scroll its pad off-screen and resize while holding Gate; playback continues
      until the original mouse-up. Scrolling does not select or trigger another pad.
- [ ] Selected, sounding, and favorite pads remain distinguishable. Empty cells and gaps
      do not trigger. Stopped playback clears the sounding highlight.
- [ ] Signal Grid uses flat warm faces, small corners and narrow gutters without
      bevels or shadows. Selection stays orange and playback has a separate cream
      rim (dark ink on neutral light-theme pads), including on pink favorites.
- [ ] Favorite pads have a pink face in both themes, including pads outside the active
      MIDI set. Hover and press transitions are smooth; released pads settle without
      a permanent flashing or pressed state.
- [ ] Single-tap auditions; double-tap toggles the exact numbered pad's favorite status.
      A double-tap does not inject an extra audition or change the active MIDI mode.
      MIDI Lock protects assigned favorites. Removing a favorite keeps its reserved note.
- [ ] The MIDI selector offers only WINDOW and FAVORITES. An older project's original
      notes remain intact under those labels until an explicit remap.
- [ ] Move the Window to later rows, including a 56-note range spanning four content
      rows. Its first mapped row is revealed within the three-row viewport. Dragging
      previews and reveals the range without remapping until release. Undo restores
      the previous map; resizing during a drag cancels its preview.
- [ ] With Favorites open, favorite pads keep that view; a nonfavorite pad returns to
      the waveform and selects/plays the exact chop.
- [ ] In FL Studio on macOS, **FL OCT** changes the host's Standard Piano typing octave
      within C4–C6 after typing keys are released. The same three-row pad window
      scrolls whole-bar and Micro chop sets.
      Other hosts do not show those shortcut buttons.

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
- [ ] Use **EXPORT CHOP** inside the callout and confirm the saved MP3 has the selected chop's
      ADSR baked in. Save/reopen the project and confirm all four values restore per chop.
- [ ] Enter manual-chop mode, complete and select a chop, then open the same ADSR/export callout.
      Changing the selection while a callout is open must not redirect its knobs to another chop.

## Cue points during grid edits
- [ ] Set the chops' **CUE** knobs to 0%, then adjust **TEMPO** and **GRID** in both
      directions and back. Select each chop: its cue stays at its new start and reads 0%.
- [ ] Repeat with **2x**, bars-per-chop changes, and Shift-dragging a chop edge.
      Zero cues stay at the chop starts, including chops with silent lead-ins.
- [ ] Set several different nonzero CUE percentages. Tempo, grid, 2x, bar changes,
      and either kind of boundary resize keep each surviving numbered chop's
      percentage unchanged. Audio starts at that percentage of the new bounds.
- [ ] Load audio with silent lead-ins and create transient chops: all new cues
      start at 0%. Only moving the CUE knob changes its setting.
- [ ] Save/reopen including very short chops and the inactive manual/automatic
      layer: exact knob values persist. Older sessions retain their saved cue
      offsets; any previously auto-generated offsets remain until manually reset.

## Manual chopping
- [ ] Press **CHOP MANUALLY** after automatic chops exist. The waveform immediately becomes
      a clean slate and shows the double-click start instruction.
- [ ] Double-click a start point, hold a hardware or DAW typing-keyboard MIDI note, and release it
      later in the audio. One numbered chop appears from the start point to the exact release
      position and keeps the MIDI note used during capture for later triggering.
      The numbered on-screen pads audition existing chops; use hardware/DAW MIDI for capture.
- [ ] Place another start marker, then double-click its end without touching MIDI. A numbered,
      sequentially MIDI-mapped chop appears and MIDI capture is cleanly cancelled.
- [ ] Create several chops out of timeline order. Labels remain numbered in waveform order,
      while each MIDI-captured chop retains its exact MIDI note assignment.
- [ ] Hover either boundary of any completed manual chop. The cursor changes to horizontal
      resize; drag the start and end markers independently and confirm the chop bounds update,
      the label follows waveform order, and its MIDI note assignment is unchanged.
- [ ] With overlapping or adjacent chops, click a chop to select it, then resize its shared or
      nearby boundary. The selected chop's marker wins when two edges occupy the same position.
- [ ] Reassign the same MIDI note to a newer chop. The newer chop owns that MIDI note; the older
      chop remains directly playable from its numbered pad, selectable and exportable by mouse.
- [ ] Start manual mode while tempo analysis is still running. Its late result must not refill
      the cleared chop list. Toggle manual mode off, save/reopen, and confirm completed chop
      bounds and MIDI assignments persist.
- [ ] While holding a hardware/DAW MIDI note during capture, confirm the live end guide follows playback and audio
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
      and shows a soft halo. Drag it — the marker moves and a subsequent MIDI-note hold auditions
      from the new position, not the original one.
- [ ] While holding a hardware/DAW MIDI note mid-capture, the marker must NOT be draggable or removable.
- [ ] Double-click the pending marker to remove it. Double-clicking anywhere else still sets
      the chop end.
- [ ] Select a completed chop in manual mode. A red **×** badge appears immediately right of
      the ADSR/EXPORT pill. Click it and the chop is removed; undo restores it.
- [ ] Press the × but release the mouse away from it — nothing is deleted.
- [ ] Scroll a selected chop to the far left and far right edges of the view. The × never
      overlaps the +/- zoom buttons and never leaves the display.
- [ ] Outside manual mode no × appears, and the ADSR/EXPORT pill sits exactly where it did
      before (position must be unchanged).
- [ ] After importing a new sample, complete a manual chop by double-clicking the end
      (no MIDI note held). It is assigned **C2**. The next such chop takes C#2, then D2.
      Delete the C#2 chop and create another — it reuses C#2 rather than continuing upward.
- [ ] Chops captured by holding a MIDI note keep that exact assignment; the auto-assigned ones skip over
      any note already taken.
- [ ] Load an older project. Its chops retain their saved root and note assignments.

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

## MIDI mapping and chronological pads
- [ ] Load a sample. Every chop header wide enough to show it carries the key that
      fires it, on the right of its number. Narrow the zoom until headers are under
      ~52 px — the note name drops, the number stays.
- [ ] Every waveform header name matches the MIDI note/channel that actually triggers
      that chop. Changing FL's typing octave leaves these assignments unchanged.
- [ ] The pad rail shows every chop in source order with numbers only. Selection,
      sounding playback and favorites remain distinguishable in both themes;
      incoming MIDI highlights the same chop as clicking its numbered pad.
- [ ] Enter manual mode and pin a chop to a key that a positional chop already
      resolves to (e.g. capture on C2 with the root at C2). The displaced chop's
      header shows a dimmed `-`. Its numbered pad remains visible and playable.
- [ ] A chop without a MIDI assignment remains selectable, directly previewable from
      its pad or waveform, and exportable by mouse.
- [ ] Clear the collision (delete or reassign the pinned chop). The dash disappears,
      and that chop's original MIDI note triggers it again. Pad order stays chronological.
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
      another (check waveform note labels for unexpected unassigned `-` markers).
- [ ] Pin two adjacent chops to different MIDI notes at 2 BARS, then switch to 1 BAR. Each
      note stays on one chop; the split twins fall back to the positional map rather
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
- [ ] With a stem muted, trigger chops via the numbered pads / MIDI — chops play the
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

Exports use 320 kbps MP3 and snapshot the chop, cue, pitch, warp,
tempo/HALF-TIME, gain, envelope, reverse and stem mutes at
the export gesture. Rendering and Save As copies run off the UI/audio threads.
The envelope follows full-velocity one-shot playback; a held MIDI gate/note-off
performance is not recorded into the file. Prepared audio preserves headroom
instead of independently normalising each chop.

- [ ] Drag the DRAG AUDIO handle with pitch, warp, SYNC and HALF-TIME individually,
      then combined. Place each MP3 on the grid and compare its onset, duration,
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
- [ ] Use ADSR > Save MP3, cancel it, and test a destination that cannot be written.
      Failed saves report an error and preserve the rendered file and any existing
      destination file. Close the editor while rendering; no crash or late dialog.
- [ ] Drag to a DAW that references files in place. Wait more than 60 seconds,
      close the plugin, save/reopen the DAW session and verify the clip still plays.
      Successful drag files stay in `CueSampler/Exports` under JUCE's user application
      data directory (macOS: `~/Library/CueSampler/Exports`). They are not automatically
      deleted; don't manually remove files still referenced by projects. Use the
      DAW's collect/copy-media feature before moving a project to another computer.

The `test_project_restore` suite also reads actual exported MP3s and checks
processing-delay compensation, loop lengths, pitch, late-loop audio, sample-rate
conversion, float peaks, cue/reverse, stem intent, long-release short chops,
background snapshots/cache lifetime, and stem-resampling alignment. Fresh stem
cache keys include the corrected resampler revision; old saved projects preserve
their embedded stems until explicitly separated again.

## Stem quality and readiness (10% overlap / sinc resampling)

- [ ] On Windows with `CUE_ENABLE_DIRECTML`, `CUE_DISABLE_DIRECTML` and
      `CUE_STEM_THREADS` unset, separate a full song while playing audio. Drag
      controls, move/resize the editor and check progress animation throughout
      first-use model loading and subsequent separations. The default uses CPU,
      half the logical cores capped at four, sleeping ORT workers and a
      low-priority separation caller. Compare playback stability at small host
      buffer sizes; the pass can take longer than GPU inference.
- [ ] For GPU comparison only, set `CUE_ENABLE_DIRECTML=1` before starting the
      host. Check UI responsiveness on the actual GPU/driver: DirectML shares
      display resources and can stall rendering. `CUE_DISABLE_DIRECTML` still
      forces CPU even when the enable flag is set. Unset the enable flag to
      restore the default. macOS provider/thread defaults are unchanged.

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
compact portable save/restore before the cache exists. That opt-in run also
keeps an editor open and checks message dispatch during model loading/inference,
reporting its worst dispatch gap (under one second required).

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

## Favorite pad colors

- [ ] Double-click a waveform chop to favorite it: both the chop and its numbered
      pad turn pink, including chops without a MIDI assignment.
- [ ] Select or play a favorite: its pad keeps its pink accent with a stronger
      highlight. Unfavorite it and confirm the normal selected/sounding styling returns.
- [ ] Undo, change FL OCT where available, switch chop layers and reopen the
      editor/project. Pink follows the saved favorite flag on the exact chop;
      MIDI collisions do not hide or transfer a favorite's pad styling.

## Favorites performance view

- [ ] While transport playback or a held MIDI chop is sounding, toggle Favorites
      on and off: audio continues. Release the original MIDI key after switching:
      GATE releases normally; ONE SHOT plays through. Check every favorite using
      tiles and MIDI after the view has been open for several seconds.
- [ ] In FL Studio, audition tiles with the host transport stopped. Mouse
      audition uses the same preview path as the waveform, including NO MIDI
      tiles beyond the available MIDI assignments. Hiding Favorites releases a held
      mouse GATE; hardware MIDI follows the displayed C2-upward assignments.
- [ ] Favorite chops out of sample order (for example 3, 1, 6, 4). Click
      **FAVORITES** above the waveform: only those chops appear, side by side,
      labeled C2, C#2, D2, D#2 in the order they were favorited. Incoming MIDI
      agrees with the tile labels. The separate numbered pad rail keeps every
      chop in source order and highlights the same favorite chop.
- [ ] Click/hold a tile in GATE mode, then release it. Try ONE SHOT and hardware
      MIDI too. Cue, gain, pitch, reverse, warp and stem settings still belong
      to the original chops. This view does not concatenate or rewrite audio.
- [ ] Double-click tiles repeatedly: they audition without changing favorite
      membership, order or MIDI assignments. Toggle Favorites off to add/remove
      favorites by double-clicking in the regular waveform view. Re-favoriting
      appends to the lineup; Undo restores the removed favorite's position.
- [ ] Play every tile using mouse and MIDI. A green playhead moves through the
      active tile, resets on loops/retriggers, and disappears when playback stops.
      Check cue offsets, reverse, warp, ONE SHOT and host sync. Selecting another
      tile or having overlapping source chops must not move/duplicate the playhead.
- [ ] Toggle Favorites off. Original explicit MIDI assignments, unassigned chops,
      octave offset, full waveform zoom/scroll and source boundaries return.
      In Favorites, octave and waveform-editing controls are disabled; favorite
      tiles wrap into rows and fit within the waveform panel without scrolling.
- [ ] Save/reopen with Favorites on, including favorites from a manual chop layout.
      The current view, favorite order and temporary MIDI map return. Switching
      it off after recall still restores the original mappings.
- [ ] Remove every favorite: show the empty-state instructions and no favorite MIDI
      assignments. The numbered pad rail still shows and auditions every source chop.
      Load another sample: Favorites returns off. Old projects without favorite
      chronology use sample order initially; new favorite actions record order.
- [ ] With 8, 18 and 24 favorites, verify every tile fits on screen in rows.
      Resize the window: labels and waveforms stay inside their tiles. Play and
      audition tiles in each row; gaps and empty cells must not trigger a chop.
      Favorite MIDI assignments begin at the saved root and retain their existing
      channel banks; any tile beyond MIDI capacity explicitly displays NO MIDI.
      Favorites are taken from the active chop layout; switch the manual/automatic
      layout in the full view. Numbered pads remain chronological in either view.

`test_project_restore` checks chronology, the real Favorites button, C2 MIDI
triggering, original-pin suppression/restoration, original audio/settings,
held-voice reset on remap, removal/re-add, Undo, portable recall and old states.

## Waveform scrollbar

- [ ] In FL Studio on macOS 15.2, use two-finger vertical trackpad scrolling
      and pinch in/out over the waveform. Zoom follows the pointer; quick,
      small movements accumulate instead of stalling. Two-finger horizontal
      scrolling pans and stays in sync with the SCROLL knob.
- [ ] Verify ZOOM - / + and FIT are absent. The ZOOM knob adjusts horizontal
      zoom; double-clicking it shows the full sample. The original +/- buttons
      still adjust waveform height independently.
- [ ] On a laptop, test slow/fast pinch in/out, two-finger vertical zoom and
      horizontal pan, including momentum and diagonal movement. The dominant
      scroll direction controls zoom or pan without minor-axis drift. Test
      Ctrl+vertical scroll, including pinch translated to Ctrl-wheel by the OS.
      Repeat in standalone and the supported macOS/Windows plugin hosts; native
      pinch delivery depends on the OS, driver and host.
- [ ] Check minimum/default/maximum editor sizes, both themes, manual chop and
      warp modes. Colour controls, hints and chop export actions remain clear.
      Save/reopen and verify zoom and scroll values return.

- [ ] Zoom in and drag the horizontal scrollbar slowly, quickly and back and forth.
      The waveform eases smoothly as with wheel scrolling; the thumb stays under
      the pointer without being pulled backward by the animation.
- [ ] Release at either end and midway. The waveform settles at the requested
      position and the SCROLL knob agrees. Check track clicks, wheel scrolling,
      SCROLL knob movement, zoom changes and resizing the window too.

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
- [ ] ADSR opens its envelope menu; **SAVE MP3...** opens the file-save workflow.
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

## Chop editing frame budget

- [ ] With a long stereo sample and 32+ chops, audition while dragging CUE,
      gain, pitch and ADSR; playhead and hover animation should stay smooth.
- [ ] Warp several chops, then drag one marker. Only that chop's waveform
      changes; removing markers and undoing edits must not leave stale artwork.
- [ ] Repeat on a Retina display, resize the editor, scroll/zoom, change theme,
      and cycle waveform colour modes. Check waveform sharpness and alignment.
- [ ] Check panel shadows and favorites switching; the favorites playhead must
      still animate and double-clicking a favorite must not remove it.
- CPU paint benchmark (opt-in, no timing assertions in CI): run
  `build/test_project_restore_artefacts/Release/test_project_restore build --ui-benchmark`.
  Uses deterministic 60-second stereo audio and 32 chops at 1x/2x resolution.
  Scenarios: 0 = cue edits, 1 = cue edits on warped chops, 2 = all warp maps
  changing (stress), 3 = one warp map changing. Reports warm mean/p95/max paint
  milliseconds. The 60 FPS budget is 16.67 ms for the entire UI frame; this
  benchmark measures waveform component CPU painting, not host presentation FPS.


## Edit a favorite in the waveform

- [ ] In Favorites, select a chop and click EDIT CHOP in the header. The main
      waveform opens with that original chop selected and both edges in view.
- [ ] Repeat with favorites added out of source order, a chop near the sample
      end, a very short chop, and a manual chop. CUE/gain/ADSR edits should affect
      the correct original chop, and Favorites should retain its order.
- [ ] With no favorites (or no selected favorite), EDIT CHOP is disabled.
      Double-clicking a favorite still auditions without removing it.

## FL OCT -/+ and MIDI mapping

- [ ] In FL Studio on macOS, restart FL after installation and select its Standard
      Piano typing layout. **FL OCT** appears; no virtual MIDI input or controller
      adapter is needed. Other hosts/platforms do not show these shortcut buttons.
- [ ] Import a sample with at least 104 chops. MIDI mapping starts at C2: chop 1
      is C2, chop 13 is C3, chop 49 is C6, chop 61 is C7, and chop 104 is G10.
      Every chop remains visible as a numbered pad. With FL typing at C2, Z
      plays/highlights chop 1; clicking pad 104 plays that exact late chop directly.
- [ ] Play a typing note first to reveal its real octave. Release it; FL OCT −/+
      controls FL C4–C6 while preserving absolute MIDI assignments. Check FL's
      actual menu/notes as well as the CUE caption. At C4 the − button is disabled;
      at C6 the + button is disabled. There is no C7–C10 screen navigation.
- [ ] Hold a typing key and request an octave change: native dispatch is blocked,
      the caption stays put and the original note-off releases normally. Release,
      retry and confirm the change. Test inactive FL, text editing and dialogs;
      blocked requests must not transpose held notes or alter the pad layout.
- [ ] Record a clip using FL typing; FL OCT never reinterprets its MIDI. Clicking
      numbered pads auditions in CUE without injecting a DAW recording input.
      Save/reopen: root and mapping return; recall sends no FL shortcut.
- [ ] A 220-chop C2 sample retains chops 1–104/channel 1, 105–208/channel 2,
      and 209–220/channel 3. Later ranges restart at C2. All 220 numbered pads
      remain visible; each DAW channel/note pair and its pad trigger the same chop.
- [ ] Older C0 and C4 projects retain their existing mappings/range sizes.
      Importing a new sample starts C2; failed imports preserve the old map.
      Manual captures retain explicit pins; new unpinned manual chops start C2.
- [ ] Favorites pack from the current saved root. Test both themes and minimum
      editor size: FL OCT and blocked-request status fit the mapping strip where
      available. The CHOPS slider stays absent; later chops keep their DAW
      channel/note assignments and all source-ordered pads remain visible.

`test_project_restore --octave-banking` covers the C4–C6 pad shortcut limits,
blocked requests, C2/104-note channel ranges and recall, legacy C0/C4 mappings,
all 128 legacy MIDI notes, Favorites and release pairing. Validate actual native
FL shortcut delivery in the host after installation.

## Recoverable sample trim

- [ ] Select a waveform chop and click its TRIM SAMPLE action beside DRAG AUDIO.
      It immediately applies that chop's logical start/end as the sample bounds;
      cue, reverse, warp and Tail do not alter the crop. No export or audition is
      started. Repeat on an already trimmed source to verify original offsets.
      The full original stays recoverable and Favorites clears after completion.
      Releasing outside the button or changing selection during the press cancels;
      MIDI Lock blocks the action. A double-click does not audition a pad, toggle
      a favorite or queue another crop after the first click is accepted.
- [ ] Click TRIM SAMPLE. Drag START / END and edit their seconds fields. CANCEL
      restores the same audio, chops, Favorites and MIDI assignments.
- [ ] While Trim is open, use the normal Play button to loop the proposed range.
      Adjust either endpoint while listening: playback stays inside the current
      range. Pending seconds-field edits apply when Play is pressed. Pause keeps
      the audition position; Stop returns to the proposed Start.
- [ ] Reopen an already trimmed sample and audition a range outside the current
      crop. Listen to the retained original, without old chop cue / ADSR / Tail /
      reverse / warp / pitch / sync changes. Master output gain still applies.
      The green playhead and time display use the original sample timeline,
      including when the waveform is zoomed or panned.
- [ ] Cancel, Done, editor hide/close, import and project restore stop the trim
      audition. A later pad or MIDI trigger takes over normally; returning to
      Play while Trim remains open resumes the range audition. Previewing never
      changes the sample, chops, Favorites or recorded MIDI assignments.
- [ ] Use the trim header's −/+ buttons, vertical wheel or pinch to zoom. Wheel /
      pinch zoom anchors to the pointer. Pan with horizontal / Shift-wheel or drag
      the viewport in the full-original overview below the waveform. FIT shows
      the full original without changing either trim point.
- [ ] Zoom closely into each endpoint, pan to the other end and drag it precisely.
      The seconds fields retain original-source coordinates. Numeric edits reveal
      an endpoint outside the current view. FULL SAMPLE resets both bounds and view.
- [ ] Apply a shorter range with DONE. Existing audio remains available during
      preparation, then BPM/key analysis and automatic chopping run on the range.
      Current BARS / Micro-Chops preference stays selected; Window starts at chop 1,
      Favorites clears, and previous chop edits / tempo fine trim / offset reset.
- [ ] Open TRIM SAMPLE again. The full original is visible. Expand either edge,
      or choose FULL SAMPLE and DONE, to recover removed audio exactly.
- [ ] Save/reopen while trimmed with the original file unavailable. Both the
      selected range and full-original recovery remain available. Repeat after
      muting stems; trim starts from the original unmuted audio.
- [ ] MIDI Lock disables applying trim. A new file / trim / project restore wins
      over an older pending trim. Closing/reopening the editor during preparation
      does not lose the result or crash.
- [ ] In FL, FL OCT minus is disabled at C4 and plus at C6. Check actual typing
      notes and release held keys before changing octave. Legacy saved MIDI maps
      retain their notes even though the shortcut controls have the new floor.

`test_project_restore --sample-import` includes trim validation; `--sample-trim`
runs the focused trim checks. These cover exact original slices, cancellation,
Favorites clearing, fresh analysis, stale work and portable recovery.

## Per-chop tail length

- [ ] Open a chop's ADSR menu. TAIL LENGTH offers OFF / 50 / 100 / 200 / 300 ms.
      Each chop keeps its own value; OFF preserves existing playback and exports.
- [ ] In ONE SHOT, hear the original attack and body followed by extra source
      audio fading to silence. Check a chop near the end of the sample: the tail
      stops safely at available audio and reaches silence without a hard cut.
- [ ] In GATE, hold through several loops: loop timing stays unchanged. Release
      partway through a loop: audio continues from that position for the selected
      tail fade. With Tail enabled this fade replaces the Release knob; OFF uses
      the normal ADSR release. A new pad still replaces the previous chop.
- [ ] Test reverse with a nonzero cue, warped chops, SYNC, half-time, pitch and
      different host rates. The attack stays in place and tail milliseconds refer
      to audible output time. Reverse continues backward, bounded by the source.
- [ ] Drag audio and SAVE MP3 include the same One Shot tail and fade. Editing
      Tail invalidates prepared exports. Save/reopen, Undo and switch manual /
      automatic layers: values stay attached to their chops and recorded notes
      retain their assignments. Old projects start with Tail OFF.

## Background sample import

- [ ] Load a long WAV/MP3 using both the file chooser and drag-and-drop. Controls
      remain responsive while the sample is prepared; existing audio keeps playing
      until the replacement is ready.
- [ ] Import two files quickly. The last selection wins. Restore a project while
      an import is pending: the restored sample must remain selected.
- [ ] During import, the waveform shows an animated loading panel with the
      incoming filename and progress. Use CHOOSE ANOTHER SAMPLE or drop a new
      file onto that panel: the filename updates and the latest file replaces
      the old audio. REPLACE SAMPLE remains available after loading finishes.
- [ ] Drop a corrupt or unsupported audio file. A clear error appears, the
      loading panel clears, and the previous sample remains available.
- [ ] Close/reopen the editor during import, and remove the plugin during import.
      No crash or callback into a deleted editor.
- [ ] Save immediately after the new waveform appears, then reopen with the
      original audio file unavailable. Embedded audio restores correctly.
- [ ] Import a long file and begin playing as soon as the waveform appears.
      Project-audio compression continues in the background. Save immediately,
      then again after a few seconds: both saves recall without the original
      file; the later save uses compact embedded audio.

`test_project_restore --sample-import` covers background dispatch, latest-file
selection, failed loads, embedded audio readiness and project-restore cancellation.
It also exercises loading-panel file drops and block-by-block encoding cancellation,
and writes `build/sample-import-loading.png` for layout inspection.
It verifies immediate recall with compression blocked and the source file deleted,
then verifies compact recall and shared persistence through stem remixes.
`test_project_restore build --import-benchmark` compares median preparation time
for a 30-second stereo WAV over three runs against the previous import path.


## Grid and pad visibility

- [ ] In automatic and manual chop modes, orange bar guides use round dots with
      clear gaps. Zoom/scroll and adjust TEMPO: dots stay aligned to the bar grid,
      and no continuous beat line shows through the gaps.
- [ ] Check both themes at normal and minimum editor sizes. Every chop has a
      source-ordered, number-only pad. Selected pads have an accent outline;
      sounding pads have a bright orange fill (pink for favorites), and favorite
      accents remain visible even when the chop has no MIDI assignment.
- [ ] Release a Gate pad: its sounding fill clears after release and selection/
      favorite styling remains. One Shot keeps its sounding fill until playback
      ends. Host MIDI and direct pad clicks highlight the same sounding chop.
- [ ] Load more than 128 chops, toggle Favorites and reopen the editor. The CHOPS
      slider stays absent and every chop stays visible/playable. FL OCT is shown
      only in FL Studio on macOS and stays bounded to typing C4–C6; existing
      DAW MIDI channel/note assignments still trigger the same chops.
