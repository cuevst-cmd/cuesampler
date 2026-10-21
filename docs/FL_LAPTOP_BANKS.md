# Keyboard and MIDI

New samples use **WINDOW**, starting at **C4 (MIDI 48)**. At FL Studio's default
typing octave, the first key plays chop 1 immediately. The window assigns 56
consecutive chops to C4-G8. Source chops appear as numbered pads in song order.
The MIDI selector has exactly two choices: **WINDOW** and **FAVORITES**.

**MICRO-CHOPS**, immediately to the left of MIDI, offers **OFF**, **1/2 BAR**,
**1/4 BAR**, **1/8 BAR** and **1/16 BAR**. These are fractions of one bar,
independent of the remembered 1/2/4/8-bar choice. At 120 BPM in 4/4, they produce
chops of **1000 / 500 / 250 / 125 ms**, respectively. OFF returns to the remembered
BARS choice; clicking any BARS button also exits Micro-Chops. Changing the
division rebuilds the automatic chops, resets Window to chop 1 and is undoable.
MIDI Lock and manual chopping disable it. The selected division is saved with
the project; a new import retains that denominator and uses bar fractions.

Projects from the beat-based version show a current-only value such as
**1/8 BEAT\***. Their saved grid, MIDI assignments and beat-based tempo edits stay
intact until an explicit BAR choice or BARS button rebuilds the grid. Selecting
the same denominator as a BAR choice also converts it. One Undo restores the
previous beat grid and its assignments. Original projects without a saved unit
continue to use bar fractions.

All chop modes use the same vertically scrollable pad window. Pads stay at least
48 px wide and 40 px high, with at most three visible rows (120 px). Scroll with
the wheel / trackpad or scrollbar to reach later chops. Scrolling moves whole
rows, does not remap MIDI, and keeps a held Gate playing until mouse-up. The
waveform stops giving up height once three pad rows are visible, even for long
songs or dense Micro-Chops. This also applies with Micro-Chops OFF.

Moving or dragging the Window reveals its first mapped row. The full 56-note
range can span more than three content rows; scroll to inspect the remaining
assigned pads. A resize cancels a Window drag preview without changing its map.

The flat Signal Grid uses CUE's warm charcoal and cream palette, solid pink
favorite faces, orange selection rims and a cream playback rim (dark ink on
neutral light-theme pads). The cyan region outlines the assigned set. Short
cyan bottom ticks show notes reachable at the current typing octave.
Favorite pad faces stay pink, including
chops outside the active MIDI window. Hover, press, trigger and release highlights
animate on the UI thread and settle when idle. Drag the **DRAG MIDI** handle in the pad header
to preview another consecutive range; release to commit it. This changes which
chops existing MIDI notes play. One Undo restores the previous map.

Changing **1 / 2 / 4 / 8 bars** resets Window to a consecutive range starting at
chop 1 and C4. Shorter samples map every available chop.
This also refreshes the stored Window while Favorites is active; Favorites keeps
its separate fixed slots and gaps. One Undo restores the old bars and map.

In FL Studio on macOS, **FL OCT -/+** controls the Standard Piano typing octave
within C4-C6. The minus button is disabled at C4. Release typing keys before changing OCT. A blocked request leaves
the octave unchanged. The installed Piano layout spans 32 chromatic notes;
starting at C4 and stepping through C6 reaches the complete 56-note set.
Changing octave does not reassign chops.

For a pattern using distant chops, add them to **Favorites** in the desired
order, then select **USE FOR MIDI** or choose **FAVORITES** in the MIDI selector.
For example, favoriting chop 1 then chop 200 assigns C4 then C#4. Removing a
favorite leaves a reserved empty slot. New favorites append; re-adding the same
chop restores its reserved note. Excess favorites remain mouse-playable without
an automatic MIDI-channel bank. **SLOTS... > Rebuild slots** explicitly reclaims
gaps and changes recorded note meanings; Undo restores the previous layout.
Double-tap a numbered source pad to toggle its favorite status. Single presses
still audition normally, and MIDI Lock protects assignments from double-tap edits.

Opening Favorites or returning to the waveform only changes the view. The
active MIDI performance set stays in place. Every pattern on the same plugin
instance uses that set; use another instance for an independent set.

Enable **MIDI LOCK** to prevent moving the Window, changing its MIDI source,
switching chop layers, rebuilding the grid, deleting assigned members, or
rebuilding favorite slots. Browsing, direct pad audition and intentional sound
edits remain available. Unlocking alone does not change assignments.

**TRIM SAMPLE** opens the original waveform with START / END handles and exact
time fields. **CANCEL** discards edits; **FULL SAMPLE** expands the proposed range.
**DONE** applies the range, reanalyzes BPM and rebuilds chops using the current
BARS / Micro-Chops setting. It clears Favorites and old chop edits, resets tempo
fine trim / grid offset and starts Window at chop 1. The full original remains
recoverable, including after saving and reopening. Unlock MIDI before trimming.
Use the trim header's −/+ or wheel / pinch to zoom; pan with horizontal scrolling
or the overview strip. FIT shows the full original without changing trim bounds.

Each chop's **ADSR** menu includes **TAIL LENGTH: OFF / 50 / 100 / 200 / 300 ms**.
One Shot adds a fading continuation after the original chop. Gate keeps its loop
timing and uses the tail fade when released; OFF uses the normal Release knob.
Tail duration is in audible milliseconds and is limited by available source audio.
The next pad still replaces the previous chop. Exports and projects retain tails.

Projects save the active map, Window bindings, Favorites reservations, both chop
layers, typing-octave view and MIDI Lock. Older projects retain their saved root,
explicit assignments, channel banks and saved Favorites map. These appear under
Window or Favorites without additional selector modes. Opening an old project
never silently changes its recorded notes. Refreshing / moving its Window or
choosing USE FOR MIDI explicitly adopts the new C4 layout; Undo restores the old map.

Direct pad clicks audition inside CUE using Gate / One Shot. Mouse-click input
lighting and recording inside FL is a separate host integration requirement;
this feature does not establish that input path. No controller adapter or
virtual MIDI source is installed by the Window / Favorites feature.

See [the performance mapping specification](CHOP_MIDI_PERFORMANCE.md) and
[FL Studio keyboard shortcuts](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/basics_shortcuts.htm).
