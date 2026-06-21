## CUE SAMPLER 1.0.1 — Windows (VST3, 64-bit)

Download **`CUESAMPLER-Setup-1.0.1.exe`** and run it. The plugin installs to
`C:\Program Files\Common Files\VST3` — rescan plugins in your DAW if it doesn't
appear right away.

### What's new in this build
- **Faster load:** the stem-separation model is now loaded on demand instead of
  at startup, so the plugin instantiates instantly in your DAW.
- **Manual stem separation:** click **SEPARATE** in the STEMS panel to split a
  sample into drums / bass / vocals (no longer runs automatically on every load).
- **Update checker** now links directly to the Windows installer.

### Note on the SmartScreen warning
This installer is **not yet code-signed**, so Windows SmartScreen may show
"Windows protected your PC". Click **More info → Run anyway** to proceed. The
`.sha256` file is included so you can verify the download.
