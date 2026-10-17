# Keyboard and MIDI

New samples start at **C2 (MIDI 24)**: chop 1 = C2, chop 2 = C#2,
chop 13 = C3, chop 49 = C6, chop 61 = C7, and chop 104 = G10 (127).
The screen keyboard opens at C2. FL and CUE use the same absolute MIDI pitches
and note labels. Explicit manual assignments remain fixed.

In FL Studio on macOS, **OCT −/+ controls FL's typing octave within C2–C6**
using its documented Option + numeric-keypad 2–6 shortcuts. Use FL's standard
Piano layout. Play a typing note first so CUE can reveal the actual input octave;
release it before changing OCT. A blocked native request leaves CUE's view intact.
Held keys, inactive FL, text editing and dialogs block native shortcut dispatch.

**Above C6, OCT browses CUE's on-screen keyboard through C10.** The readout
changes to SCREEN KEYS. Click those keys to audition the higher chops; FL's
computer-keyboard octave stays at its previous setting. Returning from C7 to
C6 sends the native C6 shortcut again. Clips and typing input do not pull CUE
out of an explicitly selected higher screen view. Screen key lights and MIDI
labels always refer to the actual note, never a local playback transpose.

The normal two FL typing rows can play notes above the selected starting octave.
This does not extend FL's native octave shortcuts beyond C2–C6. Screen-key
clicks audition inside CUE; they do not inject notes into FL's recording input.

Reference: [FL Studio keyboard shortcuts](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/basics_shortcuts.htm).

Each C2-root range has **104 notes, C2–G10**. With more chops, CHOPS selects
later screen-keyboard MIDI channels. A 220-chop sample uses ranges 1–104 on
channel 1, 105–208 on channel 2, and 209–220 on channel 3. Selecting a range
returns the view to its first C2. Host MIDI keeps its original channel/note,
regardless of OCT or CHOPS. The slider is hidden with 104 or fewer chops.

Projects retain their saved root, channel/note map, OCT view and selected range.
Existing C0 projects keep their 128-note ranges; existing C4 projects keep their
80-note ranges. They are not silently reassigned to C2. Their lower screen keys
remain available. Importing a new sample resets the root/view/range to C2,
channel 1. Failed imports preserve existing state. Legacy projects with channel
aliases do not expose an overflow slider. Favorites pack from the saved root.

Obsolete learned-transposition settings remain ignored. Recorded clips play
their actual saved pitches. Opening an editor, loading a project or changing
CHOPS never sends an FL octave shortcut. FL's typing octave is shared across
plugins; only an explicit OCT click in the native range changes it.

There is no virtual MIDI source, keyboard interception or controller adapter.
The temporary full-range MIDI probe was stopped without changing FL settings.
