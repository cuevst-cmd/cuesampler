# CUE SAMPLER 1.10

Internal version: **1.10.0**. Windows x64 unsigned local candidate.

Changes since the Windows 1.1.0 release:

- **WINDOW and FAVORITES MIDI modes.** WINDOW maps 56 consecutive chops to C4–G8 (MIDI 48–103). FAVORITES provides stable note slots for your chosen chops. Existing projects retain their saved MIDI mappings.
- **Scrollable pads and a draggable MIDI window.** Scroll through numbered pads without remapping notes. Drag **DRAG MIDI** to preview and commit another consecutive chop range; one Undo restores the previous map. The pad area uses at most three visible rows, leaving room for the waveform.
- **MICRO-CHOPS in bar fractions.** Choose 1/2, 1/4, 1/8 or 1/16 bar independently of the remembered BARS setting. OFF returns to that setting. Division changes are undoable and saved with the project.
- **Recoverable sample trimming.** Audition start/end edits before applying them, restore the full original range, or trim directly to a selected chop. Applying a trim reanalyzes BPM, rebuilds chops and clears Favorites while retaining the original audio.
- **Per-chop Tail Length.** Choose OFF, 50, 100, 200 or 300 ms of fading source audio after a One Shot ends or a Gate is released. Tails use available source audio and are included in processed MP3 exports.
- **Background imports and MP3 exports.** Sample loading, chop rendering and export saving run on workers. Use **DRAG AUDIO** or **SAVE MP3...** to export processed chops with their envelope and tail.
- **Revised BPM analysis.** Analysis checks representative sections, uses Beat This! to corroborate uncertain results, and improves half/double-tempo selection, beat timing and resampling. Grid chopping continues to use one BPM and four beats per bar.

This is a local Windows candidate; build, test and installation results are recorded separately. FL Studio typing-octave control, native laptop-key capture and the recording Link are not included as Windows features.
