# CUE SAMPLER 1.0.8

## New

**One-Shot / Gate chop playback modes**
Every chop can now play in **GATE** (loops from its cue point while the note is
held, then releases cleanly) or **ONE SHOT** (ignores note-off and plays from the
cue point to the end exactly once). Retriggering during a One-Shot chokes the
previous voice, so chops never overlap. The mode is saved with the project;
projects made before this release open in GATE mode.

**Per-chop ADSR and chop export**
Each chop gets a floating **ADSR / EXPORT** pill that opens a compact callout with
Attack, Decay, Sustain and Release. Envelopes are independent per chop and restore
with the project. **EXPORT CHOP** writes a WAV with that chop's envelope baked in.
Dragging the pill still drags the chop straight into your DAW.

**Two chop layers — automatic and manual**
**CHOP MANUALLY** now toggles between the automatic chop set and your manual one
instead of replacing it. Both layers persist across save/reload, survive closing
the plugin window, and never leak chops into one another.

**Manual chopping workflow**
An orange ghost line tracks the cursor, start markers can be dragged or removed
before capture, completed chops can be resized from either boundary, and a
selected chop shows a red **×** delete badge. Chops finished without holding a pad
default to **C3**.

**Cue point follows the start marker**
Dragging a chop's start edge carries its cue point along, clamping safely when the
chop gets shorter than the cue offset. Dragging the end edge leaves the cue put.
Behaviour is now identical inside and outside manual mode.

## Changed

**WARP is easier to discover**
The full-screen guide is gone. WARP mode now shows an inline purple hint bar that
updates as you drag, the **?** button opens the warp-specific guide while the mode
is active, and chops carrying warp markers get a small purple wave glyph.

## Downloads

| Platform | File |
|---|---|
| macOS | `CUESAMPLER-1.0.8.pkg` |
| Windows | `CUESAMPLER-Setup-1.0.8-UNSIGNED.exe` |

Each installer ships with a `.sha256` sidecar you can check against the download.

## Compatibility

**macOS 11 (Big Sur) or newer**, Intel and Apple Silicon (universal binary).
Installs **VST3** and **Audio Unit** into the system plug-in folders. Signed with
a Developer ID certificate, notarized by Apple, and stapled — it opens without a
Gatekeeper warning.

**Windows 10/11, 64-bit.** Installs **VST3**. Stem separation runs on any DX12
GPU via DirectML.

> **Windows installer is not code-signed yet.** SmartScreen will show
> *"Windows protected your PC"* when you run it. Click **More info**, then
> **Run anyway**. If you want to confirm you have the real file first, compare it
> against `CUESAMPLER-Setup-1.0.8-UNSIGNED.exe.sha256` in PowerShell:
>
> ```
> Get-FileHash .\CUESAMPLER-Setup-1.0.8-UNSIGNED.exe -Algorithm SHA256
> ```
