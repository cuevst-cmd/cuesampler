# Chop window and Favorites performance mapping

Implementation specification, October 20, 2026. FL Studio testing belongs to the user.

## Agreed behavior

- Keep every source chop visible, numbered in song order, within the current window height.
- All source pads remain directly playable with the mouse using Gate / One Shot.
- Provide a draggable highlight over the consecutive chops assigned to laptop MIDI.
- Favorites combine chops from anywhere in the song into one performance set.
- Assign favorites in addition order. Append new favorites rather than sorting by song position.
- Removing a favorite leaves an empty MIDI slot. Surviving favorites keep their notes.
- Keep the active Favorites MIDI map while the user browses or edits the waveform.
- Expose only Window and Favorites in the MIDI selector. Preserve older project
  assignments internally until an explicit Window or Favorites remap.
- Give favorite pads a pink face. Animate hover, press and release on the UI
  thread, with a brief trigger flash that also catches short MIDI notes.
- Double-tapping a numbered source pad toggles its favorite status, respecting
  MIDI Lock. A single press keeps its normal Gate / One Shot audition behavior.

## One active MIDI map, independent of the view

Provide an explicit **MIDI: WINDOW / FAVORITES** selector. The existing Favorites
view button only changes what is displayed. Opening the waveform, selecting a
nonfavorite pad, and EDIT CHOP must never silently switch the active MIDI map.

MIDI notes in a DAW pattern identify a pitch and channel, not a source chop or
the window that was visible during recording. Changing the active Window or
switching the MIDI map can therefore change playback of existing patterns.
The plugin must make those changes explicit and save the chosen map with the project.

Use one active performance set per plugin instance for this first version.
Automatic and manual chop layers retain their own saved layouts; changing the
active chop layer is an explicit MIDI-map change, not just a view change.
Different patterns on the same instance share its active map. Use another instance
when a song needs an independent set; automatic per-pattern map recall is outside
this design.

## Window mapping

The laptop window covers **56 consecutive source chops**, starting at
FL C4 (MIDI 48) and ending at G8 (MIDI 103). C4 lets producers play the first
chop immediately with FL Studio's default typing octave. FL OCT can extend
the typing range upward through the rest of the set. The installed Standard
Piano typing layout spans 32 semitones per octave setting; C4-C6 settings
cover these 56 notes. It is a laptop performance range, not a general limit
of the piano roll. All other chops remain visible and can enter this range.

Examples:

| Window | First note | Next note | Last note |
| --- | --- | --- | --- |
| Chops 1-56 | C4 = chop 1 | C#4 = chop 2 | G8 = chop 56 |
| Chops 145-200 | C4 = chop 145 | C#4 = chop 146 | G8 = chop 200 |

Use exactly the same chronological sort as the numbered pads. Highlight the
actual pads in the range with cyan row bands, following row wraps. Bright cyan
top rails show the currently reachable typing octave. Keep favorite, selection and
sounding indicators distinguishable from this range outline.

Move the range with a dedicated grab handle so pad clicks keep their current
audition behavior. Snap to whole chops, preview the range while dragging, and
commit the mapping on release as one undoable edit. Clamp to the final full
window; samples with fewer than 56 chops simply map all available chops.
Display the range and its first MIDI note in the existing mapping area.

A committed Window stores its slot-to-chop bindings, not just an array offset.
Inserting a chop or moving boundaries must not silently give an existing note
to a different chop. After an edit changes the chronological order, outline the
actual assigned pads, even if they are no longer consecutive. Moving the Window
again explicitly selects a fresh consecutive range and replaces those bindings.
Keep the slot bindings on unlock; unlocking alone must not apply a new range.

The outline represents the full laptop range across FL OCT settings. A separate,
subtle indication may show the smaller range reachable at the current octave;
do not imply that 56 notes can all be played from one unchanged typing octave.

When Favorites controls MIDI, show which scattered source pads belong to that
set. The draggable Window range becomes active again only when the user
explicitly selects WINDOW as the MIDI source.

## Favorites performance mapping

Keep an explicit saved slot for each assigned favorite, separate from display
order and source pad number. With the requested C4 root:

| Action | C4 | C#4 | D4 |
| --- | --- | --- | --- |
| Favorite chop 1, then chop 200 | Chop 1 | Chop 200 | Empty |
| Add chop 37 | Chop 1 | Chop 200 | Chop 37 |
| Remove chop 1 | Empty | Chop 200 | Chop 37 |

New favorites append after the last allocated slot. Never automatically fill
reserved gaps or compact the set. Re-adding the same chop will
restore its reserved slot. An explicit layout reset can reclaim gaps when the
user intentionally starts a new performance layout. Use **SLOTS... > Rebuild
slots (changes recorded patterns)**; one Undo restores the previous layout.

Persist the reservation's owner as well as its slot. Deleting its chop leaves a
reserved, unresolved slot; Undo can restore it. Regrid identity transfer applies
to these inactive reservations too. Never give another chop a deleted owner's
slot automatically.

Keep original chop numbers on the source pads. Show the selected chop's MIDI
assignment in the mapping area / tooltip. Favorites must retain visible empty
slots where needed so their visual positions do not suggest a different MIDI order.

An outside pad remains available for direct audition while Favorites is active.
It must not exit the performance set or silently borrow another favorite's note.
Add it to Favorites before using it in that set's MIDI pattern.

## C4 defaults and MIDI Lock

1. **Starting note:** use C4 for both Window and Favorites. This prioritizes
   immediate sound at FL Studio's default typing octave. C4-G10 would be 80
   legal MIDI notes, but its upper 24 are outside the current laptop range.
2. **MIDI Lock:** the control prevents changing the active map, dragging
   its Window, changing the root, switching chop layers, or reassigning performance
   slots while locked. Browsing, direct audition, and intentional sound edits
   remain available. Global regridding or deleting assigned chops requires
   unlocking because it can remove performance-set members. A mapping lock does
   not freeze the audio contents of an edited chop.
3. **Capacity:** this version uses one laptop-accessible Favorites
   set. Keep excess favorites bookmarked and mouse-playable, with an explicit
   no-slot status; do not silently put them on another MIDI channel.

The root and capacity are fixed for these new performance layouts. Compacting
gaps is an explicit new-layout action that changes recorded note meanings.
It is unavailable while locked. Bookmarked overflow favorites
must not acquire reserved or existing slots automatically.

## State, editing and compatibility

Persist mapping mode, Window start and bindings, Favorites root, stable slots,
reservation owners, allocation high-water mark, and any lock state with both
chop layers. Undo, redo and project recall restore
the same assignments. Build and publish the derived lookup map off the audio
thread; note handling continues to use bounded array lookups.

Preserve legacy saved roots, explicit manual note assignments, channel banks,
and legacy Favorites mapping. Opening an old project must not opt it into the
new layout or change its recorded notes. New Window / Favorites modes override
the derived map without destroying those legacy assignments.

Regridding currently creates new chop IDs and can duplicate favorite metadata
across split children. Transfer performance slots separately, one-to-one: a
split follows the old cue point; a merge keeps one deterministic owner and
leaves other slots empty. Never fill an unresolved slot with unrelated nearby
audio. Ordinary boundary / cue edits intentionally change that chop's sound.

## Validation before installation

- Window mapping and clamping at 0, 1, 55, 56, 57 and 220 chops, including
  unsorted internal storage and the final song range.
- Distant Favorites, addition order, removal gaps, re-addition and capacity.
- Unchanged assignments when switching views or selecting outside pads.
- Gate note-off pairing and One Shot playback across a committed map change.
- Lock behavior, one-step drag undo, both chop layers and save / reopen.
- Legacy root / explicit-note / channel-bank / Favorites project recall.
- Split / merge ownership without duplicate or unrelated slot assignments.
- Pad outlines, drag hit targets and readable numbers in both themes at the
  existing plugin height.
- Exactly two selector options, safe double-tap favorite edits, persistent pink
  pad faces in either MIDI mode, and hover / press / release settling while idle.

Mouse clicks lighting FL's piano-roll key and recording host notes remain a
separate host-input integration requirement. This mapping design does not
establish that path. The user will perform FL Studio checks; plugin tests and
build validation can run without controlling the DAW.
