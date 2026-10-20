# Keyboard and MIDI

New samples use **WINDOW**, starting at **C4 (MIDI 48)**. At FL Studio's default
typing octave, the first key plays chop 1 immediately. The window assigns 56
consecutive chops to C4-G8. All source chops remain visible as numbered pads.
The MIDI selector has exactly two choices: **WINDOW** and **FAVORITES**.

The cyan region shows the assigned set. Bright cyan top rails show notes
reachable at the current typing octave. Favorite pad faces stay pink, including
chops outside the active MIDI window. Hover, press, trigger and release highlights
animate on the UI thread and settle when idle. Drag the Window handle in the pad header
to preview another consecutive range; release to commit it. This changes which
chops existing MIDI notes play. One Undo restores the previous map.

Changing **1 / 2 / 4 / 8 bars** resets Window to a consecutive range starting at
chop 1 and C4. Shorter samples map every available chop.
This also refreshes the stored Window while Favorites is active; Favorites keeps
its separate fixed slots and gaps. One Undo restores the old bars and map.

In FL Studio on macOS, **FL OCT -/+** controls the Standard Piano typing octave
within C2-C6. Release typing keys before changing OCT. A blocked request leaves
the octave unchanged. The installed Piano layout spans 32 chromatic notes;
starting at C4 and stepping through C6 reaches the complete 56-note set.
Lower octaves do not reassign chops. There is no higher screen-keyboard page.

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
