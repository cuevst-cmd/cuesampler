# Chop window and Favorites performance mapping

Implementation specification, October 20, 2026. FL Studio testing belongs to the user.

## Agreed behavior

- Keep source chops numbered in song order within the current window height.
  Use comfortable pads in at most three visible rows, with vertical scrolling for
  the remaining whole-bar or Micro chops.
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
bottom ticks show the currently reachable typing octave. Keep favorite, selection and
sounding indicators distinguishable from this range outline.

Use the approved Signal Grid style: flat CUE foundation faces, small corners,
narrow gutters and stable-contrast numbers. Favorites use a solid CUE pink face;
selection has an orange inner rim and sounding/trigger flash has a separate cream
rim (dark ink on neutral light-theme pads). Keep the MIDI outline outside the
faces. No pad gradients, bevels, drop shadows or continuous idle glow are needed.

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

An explicit global regrid (including 1 / 2 / 4 / 8 bars per chop) creates a new
set of chops. Reset Window to a consecutive range starting at chop 1 and C4.
Do this even while Favorites controls MIDI, so returning to Window
finds a usable range. Window slots do not compete with Favorites for descendants.
One Undo restores both the old grid and its exact assignments. MIDI Lock blocks
the regrid. Ordinary chop edits and project recall retain their saved bindings.

The outline represents the full laptop range across FL OCT settings. A separate,
subtle indication may show the smaller range reachable at the current octave;
do not imply that 56 notes can all be played from one unchanged typing octave.

When Favorites controls MIDI, show which scattered source pads belong to that
set. The draggable Window range becomes active again only when the user
explicitly selects WINDOW as the MIDI source.

## Micro-Chops and header controls

Keep the MIDI selector's Window / Favorites text at 14 px in design coordinates.
Place a separate Micro-Chops dropdown to its left, using OFF and 1/2, 1/4, 1/8,
1/16 BAR. The readable DRAG MIDI label and grip identify the existing Window
handle; show LOCKED or MIDI SET when that action is unavailable. Keep the current
editor height. Keep all pads at least 48 px wide and 40 px high. Show at most
three pad rows in a 120 px viewport, with up to 36 px for its header. Short sets
retain the existing minimum panel height. Adding more than three rows never
shrinks the waveform further; scroll the remaining content for every chop mode.
Wheel / trackpad scrolling and the scrollbar change the view only, without
changing the active MIDI map or releasing a held Gate pad. Accumulate fractional
trackpad deltas and move in whole 40 px rows, keeping the thumb aligned and
avoiding a partial fourth visible row. A held Gate also survives resizing.
Window commits and previews reveal their first mapped row; a 56-note range may
span more content rows than fit at once. Resizing cancels an active Window drag
without committing new assignments.

Micro divisions are absolute fractions of one bar, independent of the remembered
whole-bar setting. At 120 BPM in 4/4, 1/2, 1/4, 1/8 and 1/16 BAR produce chops of
1000, 500, 250 and 125 ms. The existing grid convention remains four beats per
bar. OFF restores the whole-bar setting; selecting any BARS button exits
Micro, including the already remembered value. Clear whole-bar button latches
while Micro is active. Micro edits rebuild automatic chops, preserve the normal
Window / Favorites regrid rules, and participate in grid Undo, host state and
project recall. Disable them for MIDI Lock and manual chopping. Import preserves
the denominator and uses bar fractions; projects lacking Micro default to OFF.

Persist the timing unit as `microChopUnit` (`bar` or `beat`). A saved nonzero
division without this unit belongs to the original bar-fraction grid. Projects
from the beat-based version retain their exact chop boundaries, IDs and MIDI
assignments, including during tempo edits or same-source trimming. Show a
disabled current-only dropdown item such as `1/8 BEAT*`, with an explanatory
tooltip. The four normal choices are BAR fractions. An explicit BAR choice,
including the same denominator, or a BARS button rebuilds using bar timing.
One Undo restores the saved beat unit, grid and assignments. A genuinely new
source/import uses bar timing with the remembered denominator.

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
   FL OCT controls stay within C4-C6; the minus button is disabled at C4.
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

## Recoverable sample trim

TRIM SAMPLE opens the full retained original, with draggable start/end handles
and exact second fields. Changes remain local until Done. Cancel leaves audio,
chops, Favorites and MIDI assignments unchanged. Full Sample restores the full
range in the editor and still requires Done to apply.

Done prepares the selected range off the audio thread, reanalyzes BPM/key and
rebuilds automatic chops using the current BARS / Micro-Chops choice. It resets
tempo fine trim and grid offset, clears Favorites and old chop edits, and resets
Window to chop 1. MIDI Lock prevents applying a trim. New imports, trims and
project restores supersede stale preparation work.

Keep the original audio embedded once with the selected original-coordinate
trim bounds. Save/reopen must preserve both the playing range and the ability
to expand it. Trim recovery is through TRIM SAMPLE, rather than chop Undo.

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

Regridding creates new chop IDs. Transfer Favorites reservations and bookmarks
one-to-one: a split follows the old cue point; a merge keeps one deterministic
owner and leaves other favorite slots empty. Never fill an unresolved favorite
slot with unrelated nearby audio or duplicate a favorite across split children.
Rebuild Window independently as described above. Ordinary boundary / cue edits
intentionally change that chop's sound.

## Validation before installation

- Window mapping and clamping at 0, 1, 55, 56, 57 and 220 chops, including
  unsorted internal storage and the final song range.
- Distant Favorites, addition order, removal gaps, re-addition and capacity.
- Unchanged assignments when switching views or selecting outside pads.
- Gate note-off pairing and One Shot playback across a committed map change.
- Lock behavior, one-step drag undo, both chop layers and save / reopen.
- Legacy root / explicit-note / channel-bank / Favorites project recall.
- Split / merge ownership without duplicate or unrelated slot assignments.
- Repeated 1 / 2 / 4 / 8 bar changes keep Window consecutive, including a moved
  range resetting to chop 1, fewer than 56 chops, inactive Window, Undo and recall.
- Pad outlines, drag hit targets and readable numbers in both themes at the
  existing plugin height.
- Whole-bar and Micro sets share the three-row viewport. First, middle and final
  pads remain reachable in 220- and 512-chop songs, waveform height stops shrinking,
  and scrolling preserves MIDI maps, held Gate playback and exact pad gestures.
- Exactly two selector options, safe double-tap favorite edits, persistent pink
  pad faces in either MIDI mode, and hover / press / release settling while idle.

Mouse clicks lighting FL's piano-roll key and recording host notes remain a
separate host-input integration requirement. This mapping design does not
establish that path. The user will perform FL Studio checks; plugin tests and
build validation can run without controlling the DAW.
