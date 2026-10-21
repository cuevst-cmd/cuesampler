# CUE SAMPLER 1.10.1

Windows FL Studio fixes:

- Restored **FL OCT −/+** controls on Windows. They select FL's Standard Piano
  typing octave from C4 through C6 while preserving saved chop/MIDI assignments.
- Clicking the waveform, pads and non-text controls keeps laptop-keyboard notes
  with FL Studio. Text fields retain intentional typing focus.
- Piano key events continue through the host's normal MIDI path; CUE does not
  generate a second local copy of the notes.

The Windows focus regression, full editor/project suite and sample-import checks
passed. The user also confirmed both keyboard playback inside CUE and OCT changes
work in FL Studio 2026 after installation.

This installer is an unsigned Windows x64 local candidate.
