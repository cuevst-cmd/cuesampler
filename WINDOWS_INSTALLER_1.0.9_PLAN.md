# Codex handoff: CUE SAMPLER 1.0.9 Windows installer

Prepared September 29, 2026. Execute this plan on the Windows VM.

## Objective and installer choice

Build and validate the **Windows x64 VST3-only 1.0.9 beta EXE installer** using the existing **NSIS (Nullsoft Scriptable Install System)** workflow. NSIS is the open-source installer builder we already use. Do not switch to a paid installer builder or recreate the packaging system.

Official references: [NSIS](https://nsis.sourceforge.io/Main_Page), [downloads](https://nsis.sourceforge.io/Download). The repository documents NSIS 3.12 or newer, and CI pins 3.12. Prefer 3.12 for CI parity; record the actual compiler version if using a newer release.

The installer builder does not require a paid activation key. Windows Authenticode signing is a separate requirement of the repository's commercial-release workflow; the Mac signing certificates do not supply Windows signing. Build the unsigned test candidate first. Do not bypass the existing release gates if a Windows certificate is unavailable.

## Context to preserve

- The reviewed source baseline is `main` at `7243b8a4778b1da9fb13026c3ff5ae2ad30b5038`; this document will be a later change. Transfer the intended source revision **including this handoff**, and record the actual Windows checkout commit. Do not assume the latest remote branch contains the Mac work.
- `CMakeLists.txt` already sets `project(CueSampler VERSION 1.0.9)`. Keep that version and the beta designation.
- The signed/notarized Mac 1.0.9 installer is complete. See `docs/releases/1.0.9-readiness.md`. Its successful validation does not establish Windows compatibility.
- Windows packaging already uses `make-installer-windows.ps1` and `installer.nsi`. The NSIS script contains migration logic for older **Inno Setup** installs.
- Ship VST3 only: no Windows standalone app, AU, or AAX in this installer.
- Keep the approved `EULA.md`, `PRIVACY_POLICY.md`, generated `LICENSE.txt`, third-party notices, and matching modified Bungee source archive.
- The readiness document records the owner's JUCE Starter eligibility confirmation. Preserve that context; reassess only if circumstances changed.
- This handoff creates no Windows binary and makes no claim that Windows build, signing, installation, or DAW tests passed.

Read `AGENTS.md` if present, `WINDOWS_VM_SETUP.md`, this document, `docs/releases/1.0.9-readiness.md`, `MANUAL_TEST.md`, and the current packaging scripts before changing anything. Preserve unrelated edits. Keep fixes small; introduce no audio-thread allocations, locks, or blocking work.

## 1. Prepare the Windows VM and source

1. Use a local Windows checkout, such as `C:\src\cuesampler`, rather than building from a shared Mac folder. Transfer committed source through Git or a source-only copy; do not reuse Mac build directories or CMake caches. Ensure the handoff and all 1.0.9 changes have reached the VM before building. Do not push or discard local changes automatically.
2. Install Visual Studio 2022 Build Tools with Desktop development with C++, the x64 MSVC tools, and a Windows SDK. Install Git, CMake >= 3.22, Ninja, and NSIS. CLion is optional, not required for this command-line workflow. `tar.exe` and `curl.exe` must also be available.
3. Use a Visual Studio developer PowerShell configured to target **x64/amd64**. On an Apple Silicon Windows ARM64 VM, use the ARM64-hosted x64 cross-compiler. Do not generate native ARM64 or Win32 binaries: ONNX Runtime, DirectML, and the installer payload are x64.
4. Follow the line-ending guidance in `WINDOWS_VM_SETUP.md` before FetchContent clones dependencies; preserve LF in the Bungee patch. Do not apply unrelated global Git changes without considering existing preferences.
5. Take a VM snapshot before installation/upgrade/uninstall tests and close all DAWs before replacing a plugin.

From the repository root, inspect:

```powershell
git status --short
git rev-parse HEAD
git log -5 --oneline
Select-String -Path CMakeLists.txt -Pattern 'project\(CueSampler VERSION'
cmake --version
ninja --version
Get-Command cl.exe, git.exe, tar.exe, curl.exe
# Use the actual NSIS location if different:
& 'C:\Program Files (x86)\NSIS\makensis.exe' /VERSION
```

## 2. Fetch the model BEFORE configuring

```powershell
.\download-htdemucs-model.ps1
if ($LASTEXITCODE -ne 0) { throw 'HTDemucs download failed' }
```

The gitignored `assets\htdemucs\htdemucs.onnx` must be present for a fully featured candidate. Its expected size is **316,446,953 bytes**, and SHA-256 is:

```text
68d0bf16428ef66e692cdff8a9ccf28f1ef3f69440d57e58605a4cc55fcc5e74
```

The script verifies the hash. `assets\beat_this.onnx` and its `.data` sidecar are committed and must also exist. If HTDemucs arrives after configure, reconfigure and ensure the VST3 relinks/stages the model. Do not accept an unsigned installer's missing-model warning as a complete 1.0.9 candidate.

## 3. Build Release and run the existing regression tests

Use a fresh Windows-only build directory. Run each command from the repository root, stopping on any error. PowerShell does not automatically turn every native executable's nonzero exit into a terminating error; retain the explicit checks.

```powershell
$ErrorActionPreference = 'Stop'
cmake -S . -B build-win-1.0.9 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF `
  -DCUE_BUILD_STATE_TEST=ON `
  -DCUE_BUILD_STEM_CACHE_TEST=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build build-win-1.0.9 --config Release `
  --target CueSampler_VST3 test_project_restore test_stem_cache --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }

ctest --test-dir build-win-1.0.9 -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
```

Reduce build parallelism if the VM runs short of memory. The current CMake configuration registers six tests: stem_cache, project_restore, cue_recall, manual_chops, waveform_colour, and performance_ui. Investigate failures instead of silently disabling tests; UI tests may need an interactive desktop with functioning graphics.

Confirm the bundle at:

```text
build-win-1.0.9\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3
```

Its `Contents\x86_64-win\` must contain:

- `CUE SAMPLER.vst3` (x64 PE binary with product version 1.0.9 or 1.0.9.0)
- `onnxruntime.dll` and `DirectML.dll`
- `beat_this.onnx` and `beat_this.onnx.data`
- `htdemucs\htdemucs.onnx` with the pinned hash above

Use SDK tools such as `dumpbin /headers` on the inner plugin binary to confirm x64. Current dependency pins include JUCE 8.0.12, Windows ONNX Runtime 1.20.1, and DirectML 1.15.2. Preserve these for the release unless a demonstrated blocker requires a change.

## 4. Package an unsigned development candidate

```powershell
.\make-installer-windows.ps1 -BuildDir build-win-1.0.9 -Version 1.0.9
```

Expected outputs:

```text
dist\CUESAMPLER-Setup-1.0.9-UNSIGNED.exe
dist\CUESAMPLER-Setup-1.0.9-UNSIGNED.exe.sha256
```

The wrapper checks project/binary versions, stages runtime prerequisites and legal resources, verifies the Microsoft VC++ x64 redistributable's publisher signature, invokes NSIS with warnings treated as errors, and generates the checksum. Use this wrapper instead of calling `makensis` directly.

Inspect `build-win-1.0.9\release-notices`, including the policies, dependency notices, and `Bungee-7354c0c-modified-source.zip`. Check that the ZIP includes the actual patched Bungee source and needed submodules, without Git metadata. Inspect the rendered installer license for correct text and UTF-8 punctuation.

Use a fresh output location or preserve earlier candidates before another packaging run: the wrapper temporarily uses the final filename and may overwrite an existing installer. Do not confuse stale sidecars or earlier signed outputs with the current run.

## 5. Validate installation and actual DAW behavior

Record results and distinguish x64 Windows hardware from ARM64 Windows running x64 emulation. VM success alone does not establish the complete advertised OS/DAW matrix.

- Clean install on a snapshot without a development toolchain supplying runtime dependencies. Confirm VC++ prerequisite handling, elevation, and any reboot request. Do not disable OS security features to force an install.
- Confirm installed plugin path: `C:\Program Files\Common Files\VST3\CUE SAMPLER.vst3`. Verify all runtime/model files and `Contents\Resources\Licenses` from the **installed** bundle.
- Confirm Windows Installed Apps shows CUE SAMPLER 1.0.9 and an uninstaller under `C:\Program Files\CUE SOFTWARE\CUE SAMPLER`.
- In available supported x64 DAWs, scan/load the installed plugin and verify version, editor/font rendering, sample loading, playback, MIDI, chopping, parameter behavior, beat analysis, and stem separation. Follow relevant checks in `MANUAL_TEST.md`, including project save/reopen, cue recall, and stem-cache restoration.
- Verify DirectML works or reports a clean CPU fallback. If needed, launch the test host with `CUE_DISABLE_DIRECTML=1` for a controlled CPU comparison; remove the override afterward. Record VM GPU/driver limitations.
- Verify the telemetry-disable initialization calls are present in the shipped build. Record Windows runtime observations separately; do not equate a source call or Mac test with proof of all Windows/host network behavior.
- Test an upgrade over a representative previous NSIS version and, if available, an older Inno Setup version. Check the legacy uninstall registration is removed, only one current Installed Apps entry remains, stale bundle files are replaced, and user samples/presets/projects survive. Mark unavailable upgrade scenarios untested.
- Test uninstall and reinstall. Confirm CUE-owned plugin/install files and registration are removed while unrelated VST3s, user data, and shared Microsoft runtime remain intact.

## 6. Build the signed distribution candidate when credentials are available

This is the existing repository release gate, not a paid installer-builder requirement. Inspect the Windows certificate store for the owner's trusted Authenticode certificate with its private key and ensure SDK SignTool is available. Never place certificate passwords, private keys, or exported certificates in the repository.

After validation, use the real certificate thumbprint:

```powershell
.\make-installer-windows.ps1 -BuildDir build-win-1.0.9 -Version 1.0.9 `
  -CommercialRelease `
  -JuceLicenseEligibilityConfirmed `
  -SigningCertificateThumbprint '<REAL_CERTIFICATE_SHA1_THUMBPRINT>'
```

The script signs the plugin, signs the embedded uninstaller during NSIS compilation, signs the final installer, and timestamp-verifies the plugin and installer before producing the release checksum. If the certificate is missing, retain the validated `-UNSIGNED.exe` and report signing as the remaining blocker; do not remove signing checks or rename it as a signed release.

Expected signed outputs:

```text
dist\CUESAMPLER-Setup-1.0.9.exe
dist\CUESAMPLER-Setup-1.0.9.exe.sha256
```

Install this exact signed candidate and verify the installed plugin and `Uninstall.exe` signatures with `Get-AuthenticodeSignature` and SDK `signtool verify /pa /all /tw`. Recheck the installed payload and DAW loading after final packaging. Compare `Get-FileHash -Algorithm SHA256` against the final sidecar; signing changes the installer bytes, so an earlier hash is invalid.

## 7. Handoff and completion record

Update `docs/releases/1.0.9-readiness.md` with Windows evidence: source commit, tool versions, target architecture, test results, installer path/size/SHA-256, signature/timestamp results, tested Windows/DAW versions, upgrade/uninstall results, and any remaining limitations. Do not overwrite the completed Mac evidence.

Report exactly which artifact is ready: unsigned testing candidate or signed distribution candidate. Keep binaries, models, certificates, and build trees out of Git. Creating/testing the installer does not authorize publishing to GitHub Releases, Gumroad, or changing customer downloads; leave publication for an explicit instruction.

## Prompt to give Codex on Windows

> Read WINDOWS_INSTALLER_1.0.9_PLAN.md and execute the Windows 1.0.9 installer plan using the existing open-source NSIS workflow. Inspect the checkout and tools, fetch the verified model, build the x64 Release VST3, run the existing tests, and create the unsigned EXE candidate first. Make only necessary small fixes and record validation. Continue with signing if the owner's Windows signing certificate is already available. Preserve the completed Mac release and approved policies. Do not publish or upload the installer. Clearly report anything that requires my manual DAW testing or missing signing credentials.
