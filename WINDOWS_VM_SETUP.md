# CUE SAMPLER — Windows VM build checklist (CLion)

> Goal: reproduce a **green Windows x64 build** of CUE SAMPLER on a Windows VM using
> CLion. The repo is already cross-platform (`CMakeLists.txt` uses FetchContent for
> JUCE / Bungee / ONNX with `if(APPLE) / elseif(WIN32)` branches, and there is a
> GitHub Actions Windows+macOS matrix). The Release VST3 build has been verified
> with MSVC on a Windows ARM64 VM targeting x64.
>
> Use the VM to fix compile errors fast (CI is too slow to iterate on). Once it's
> green, let GitHub Actions keep both platforms green on every push.

---

## 1. Install on the Windows VM

- [ ] **Visual Studio 2022 Build Tools** (or full VS 2022 Community) with the
      **"Desktop development with C++"** workload. This provides MSVC (`cl.exe`),
      the Windows SDK, and the C++ standard library. This matches CI, which builds
      with MSVC `x64`.
- [ ] **Git for Windows**, available on `PATH`. Required twice over: CMake's
      FetchContent clones JUCE / Bungee, and the Bungee patch step
      runs `git apply`.
- [ ] CMake + Ninja — **no install needed**, CLion bundles both (well above the
      3.22 minimum). If you prefer system tools, CMake ≥ 3.22 is the floor.
- [ ] **NSIS 3.12 or newer**, used by `make-installer-windows.ps1` to create the
      commercial-use-compatible Windows installer.

On an Apple Silicon VM, Windows and CLion run as ARM64 but CueSampler must still
target **amd64/x64**: the bundled ONNX Runtime, DirectML package, VST3 layout, and
installer are x64. Visual Studio's ARM64-hosted x64 cross-compiler supports this.

## 2. Before you clone — line endings (one Windows gotcha)

Git for Windows defaults to `core.autocrlf=true`, which rewrites line endings on
checkout and can make the Bungee `git apply` patch fail. Set:

```
git config --global core.autocrlf input
```

The repo's `.gitattributes` already pins `*.patch` to LF, which protects the patch
inside *this* repo. The global setting additionally protects the Bungee source that
FetchContent clones separately. Do both.

## 3. Get the code + optional large assets

- [ ] **Clone fresh from GitHub** on the VM. Do **not** share the Mac project folder
      into the VM — you'll hit path/artifact/`.idea` conflicts.
      ```
      git clone https://github.com/cuevst-cmd/cuesampler.git
      ```
- [ ] **Stem-separation model is NOT in the clone.** `assets/htdemucs/htdemucs.onnx`
      (~302 MB) is gitignored. Run `download-htdemucs-model.ps1` to fetch and
      SHA-256-verify the pinned model before configuring a stem-enabled build. The
      build skips it gracefully when absent, but commercial packaging fails closed.
- [ ] The **beat model `assets/beat_this.onnx` (+ `.data`) IS committed**, so beat
      analysis works out of the box.

## 4. Configure & build in CLion

- [ ] Open the project. In **Settings → Build, Execution, Deployment → Toolchains**,
      add/select **Visual Studio**, architecture **amd64**.
- [ ] In **CMake profiles**, use a **Release** profile (matches CI;
      `-DCMAKE_BUILD_TYPE=Release`). For faster debug iteration a Debug profile is
      fine too.
- [ ] Let the first CMake configure run — it downloads JUCE, Bungee + Eigen,
      and the ONNX Runtime zip. **Several minutes, needs internet.**
- [ ] Build the **`CueSampler_VST3`** target (or `CueSampler_Standalone`).
      `CMakeLists.txt` stages `onnxruntime.dll` + the models next to the binary
      automatically (the `elseif(WIN32)` POST_BUILD block).

### Command-line equivalent (matches CI)

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build --config Release --parallel
```

> `CUE_COPY_PLUGIN_AFTER_BUILD=OFF` skips the auto-install into the system VST3
> folder — recommended while iterating. Leave it ON (default) once you want CLion to
> drop the plugin into `C:\Program Files\Common Files\VST3\` for DAW testing.

## 5. Known risks (where to look if it goes red)

1. **Bungee under MSVC.** The checked-in compatibility shim covers the currently
   pinned Bungee commit. Re-test this first whenever that dependency is updated.
2. **Bungee patch didn't apply** → almost always line endings. Re-check section 2
   (`core.autocrlf input`) and that `.gitattributes` is present in the clone.
3. **LTO build time.** The project links `juce_recommended_lto_flags`; LTO under
   MSVC works but is slow. If link time is painful while iterating, build Debug.
4. **ONNX / model not found at runtime.** The DLL and `beat_this.onnx` are staged
   next to the binary by the POST_BUILD step; `findOnnxModel()` looks next-to-binary
   (fallback #2). If beat analysis is silent, check the log for
   `BeatThisAnalyzer: ready=YES`.

## 6. Definition of done

- [ ] `CueSampler_VST3` compiles, links, and produces a `.vst3` on the VM.
- [ ] (If models present) beat analysis loads; otherwise the autocorrelation
      fallback triggers cleanly.
- [ ] Load the `.vst3` in a Windows DAW (Reaper trial / FL demo): plugin scans, UI
      renders (font + logo), audio + MIDI work.
- [ ] Push to GitHub and confirm the `windows-latest` CI job is also green.

## 7. After the first green build

- The GitHub Actions matrix (`.github/workflows/build.yml`) builds Windows + macOS
  on every push and uploads artifacts — treat a red Windows job like a red Mac job.
- Build the NSIS installer with `make-installer-windows.ps1`, validate it in
  Windows DAWs, and Authenticode-sign the VST3 and installer before public release.
  The script packages the signed Microsoft VC++ x64 redistributable required by
  both the plugin and ONNX Runtime.

## 8. Commercial Windows release

The normal script invocation creates an **unsigned development candidate** whose
filename ends in `-UNSIGNED.exe`. It
must not be represented as the sellable release. Before a paid distribution:

- Confirm eligibility for the applicable JUCE 8 plan. The free Starter plan is
  currently available when annual revenue or funding is no more than $20,000;
  purchasing Indie or Pro is not required while Starter remains applicable.
- Install NSIS 3.12 or newer. NSIS is distributed under licences that permit
  commercial use; no paid installer-builder licence or activation key is needed.
- Install a publicly trusted Authenticode code-signing certificate with its
  private key in `CurrentUser\My` or `LocalMachine\My`.
- Run the commercial gate (replace the example with the real certificate SHA-1
  thumbprint):

```powershell
.\make-installer-windows.ps1 -BuildDir build -CommercialRelease `
  -JuceLicenseEligibilityConfirmed `
  -SigningCertificateThumbprint "0123456789ABCDEF0123456789ABCDEF01234567"
```

That mode signs and timestamp-verifies the VST3 binary and final installer,
packages all third-party notices plus Bungee's corresponding MPL source, and
only then writes the final SHA-256 sidecar. It fails closed when a confirmation,
trusted certificate/private key, timestamp, signature verification, or licensed
JUCE entitlement is missing. NSIS itself does not add a paid-tool entitlement.
