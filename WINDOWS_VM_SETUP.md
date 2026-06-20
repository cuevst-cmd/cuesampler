# CUE SAMPLER — Windows VM build checklist (CLion)

> Goal: get the first **green Windows build** of CUE SAMPLER on a Windows VM using
> CLion. The repo is already cross-platform (`CMakeLists.txt` uses FetchContent for
> JUCE / Bungee / ONNX with `if(APPLE) / elseif(WIN32)` branches, and there is a
> GitHub Actions Windows+macOS matrix). **The Windows build has never actually been
> compiled**, so treat this as a debugging session, not a one-click build. The most
> likely thing to break is **Bungee under MSVC** (see Known risks).
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
      FetchContent clones JUCE / Bungee / chromaprint, and the Bungee patch step
      runs `git apply`.
- [ ] CMake + Ninja — **no install needed**, CLion bundles both (well above the
      3.22 minimum). If you prefer system tools, CMake ≥ 3.22 is the floor.

## 2. Before you clone — line endings (one Windows gotcha)

Git for Windows defaults to `core.autocrlf=true`, which rewrites line endings on
checkout and can make the Bungee `git apply` patch fail. Set:

```
git config --global core.autocrlf input
```

The repo's `.gitattributes` already pins `*.patch` to LF, which protects the patch
inside *this* repo. The global setting additionally protects the Bungee source that
FetchContent clones separately. Do both.

## 3. Get the code + the files Git won't bring

- [ ] **Clone fresh from GitHub** on the VM. Do **not** share the Mac project folder
      into the VM — you'll hit path/artifact/`.idea` conflicts.
      ```
      git clone https://github.com/cuevst-cmd/cuesampler.git
      ```
- [ ] **Create `APIKeys.h`** — it's gitignored and the build won't compile without
      it. Placeholder keys are fine for a test build:
      ```
      copy APIKeys.h.example APIKeys.h
      ```
      (AcoustID fingerprinting just won't function with placeholders; everything
      else builds.)
- [ ] **Stem-separation model is NOT in the clone.** `assets/htdemucs/htdemucs.onnx`
      (~302 MB) is gitignored. The build **skips it gracefully** (stem separation
      disables, StemSeparator falls back; everything else works). Copy it onto the
      VM into `assets/htdemucs/` only if you want to test stems.
- [ ] The **beat model `assets/beat_this.onnx` (+ `.data`) IS committed**, so beat
      analysis works out of the box.

## 4. Configure & build in CLion

- [ ] Open the project. In **Settings → Build, Execution, Deployment → Toolchains**,
      add/select **Visual Studio**, architecture **amd64**.
- [ ] In **CMake profiles**, use a **Release** profile (matches CI;
      `-DCMAKE_BUILD_TYPE=Release`). For faster debug iteration a Debug profile is
      fine too.
- [ ] Let the first CMake configure run — it downloads JUCE, Bungee + Eigen,
      chromaprint, and the ONNX Runtime zip. **Several minutes, needs internet.**
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

1. **Bungee under MSVC — the #1 unknown.** It's a SIMD time-stretch lib that may
   assume Clang/GCC flags or intrinsics. If the build fails here first, that's
   expected; fix the MSVC compile flags/intrinsics in Bungee's usage.
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
- Optionally add the `API_KEYS_H` GitHub secret (CI falls back to the example file
  if absent), and later a Windows installer (Inno Setup / WiX) + Authenticode
  signing. See `WINDOWS_PORT_PLAN.md` Phases 5–7.
