# CUE SAMPLER — Windows Cross-Platform Build Plan (Handoff Document)

> **Purpose of this document.** This is a handoff brief for a fresh Claude session (ideally a
> strong reasoning model). It captures everything already discovered about the project so you
> don't have to re-explore, then lays out a phased, step-by-step plan to make the plugin build on
> **both macOS and Windows** from a single source tree, with **GitHub Actions** as the primary
> Windows build method. The goal is ongoing dual-platform development: every future edit should
> compile and ship for both OSes.
>
> **Status when written:** macOS build works locally. No Windows build exists yet. No file edits
> have been made — this is plan-only. The user does NOT own a Windows machine.
>
> **Author's note to the next session:** Do NOT start by editing CMakeLists.txt. Start with Phase 0
> (dependency reproducibility), because the build currently cannot run on a clean checkout on
> *either* OS. Fix that first; Windows falls out of it naturally.

---

## ✅ Progress log — Session 2 (Phase 1 COMPLETE, validated on macOS)

**Strategy chosen:** pure `FetchContent` (no local fallbacks). All deps now fetch on a clean checkout.

**CMakeLists.txt changes applied & validated (clean Release LTO build, all 3 formats, exit 0):**
- **JUCE** → `FetchContent` pinned to tag `8.0.12` (`GIT_SHALLOW TRUE`). Replaced `add_subdirectory(../JUCE)`.
  Note: local `../JUCE` was a loose folder (not a git repo), version 8.0.12.
- **Bungee** → `FetchContent` pinned to commit `7354c0c62652dd85af90fddfeec307881f3b4252` (no `GIT_SHALLOW` —
  can't shallow-fetch an arbitrary SHA). Eigen comes via Bungee's submodule (FetchContent inits it by default).
- **🔴 CRITICAL DISCOVERY — Bungee is patched locally.** The working Mac build depended on an **uncommitted
  edit** to `bungee/Stream.h` in the local checkout: it adds `Stream::reset()` and `InputBuffer::reset()`,
  which `PluginProcessor.cpp:2442` (the `bungeeResetPending` path) calls. Pristine upstream `7354c0c` has no
  such methods — a pure clean build **fails to compile** without it. Captured as
  **`patches/bungee-stream-reset.patch`** and re-applied on top of upstream via Bungee's `PATCH_COMMAND`
  (idempotent helper script `patches/apply_bungee_patch.cmake`, uses `git apply`). Verified: fresh clone →
  patch applies → `reset()` present → compiles & links.
- **ONNX Runtime 1.20.1** → `FetchContent URL` of the official prebuilt, per-platform
  (`onnxruntime-osx-universal2-1.20.1.tgz` / `onnxruntime-win-x64-1.20.1.zip`), `DOWNLOAD_EXTRACT_TIMESTAMP TRUE`.
  Drives `ONNXRUNTIME_ROOT` + `ORT_LINK_LIB`. The downloaded macOS dylib is universal2 (arm64+x86_64), a clean
  drop-in for the old local `libs/onnxruntime/` (which is now **unused** by the build — can be deleted later).
  Verified: dylib bundled into `Contents/MacOS/`, binary patched to `@loader_path/...`, re-signed, `codesign -v` OK.
- **chromaprint** → unchanged (already FetchContent + kissfft). ✅
- POST_BUILD macOS bundling/signing (VST3 + AU) wrapped in `if(APPLE)` so a future Windows configure won't choke.

**Heads-up for local dev:** the first CLion reconfigure after these changes will re-download JUCE/Bungee/ONNX
into that build tree's `_deps/` (one-time) and rebuild from scratch. A clean configure (delete the build dir's
`CMakeCache.txt`) is safest. Validation was done in a scratch dir `build-portable-test/` (gitignored).

**Also done this session (Phases 2.3/2.4, 3.1, 4 — code complete, CI not yet run):**
- **Phase 3.1** ✅ Removed the hardcoded `/Users/jerryvolpe/...` model path in `PluginProcessor.cpp`
  `findOnnxModel()`. It was redundant with fallback #3 (`juce::File(__FILE__)/assets/...`), which resolves to
  the same path on the dev machine. Search order now: (1) `../Resources/`, (2) next-to-binary, (3) `__FILE__`/assets.
- **Phase 2.3/2.4** ✅ Added `elseif(WIN32)` POST_BUILD in CMakeLists: stages `onnxruntime.dll` +
  `beat_this.onnx`(+`.data`) next to the VST3 **and** Standalone binaries (loop over both targets). This hits
  `findOnnxModel()` fallback #2 (next-to-binary), and Windows resolves the DLL from its own dir. The macOS
  bundling/signing stays in the `if(APPLE)` branch.
- **CI hygiene** ✅ `COPY_PLUGIN_AFTER_BUILD` is now driven by `option(CUE_COPY_PLUGIN_AFTER_BUILD ON)`; CI
  passes `-DCUE_COPY_PLUGIN_AFTER_BUILD=OFF` so the runner never writes to a system plugin dir. Verified
  locally: with OFF, 0 install steps run; build otherwise identical (regression-clean, signature OK).
- **Phase 4** ✅ `.github/workflows/build.yml`: `windows-latest` + `macos-latest` matrix. Steps: checkout →
  `lukka/get-cmake` → `ilammy/msvc-dev-cmd` (Windows) → generate `APIKeys.h` (from `secrets.API_KEYS_H`, else
  copy `APIKeys.h.example`) → `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF`
  → build → upload `CueSampler_artefacts/Release/{VST3,AU,Standalone}`. **Note:** ONNX is fetched by CMake now,
  so NO manual ONNX download step is needed (simpler than Appendix B). YAML validated.

**⚠️ UNVALIDATED — needs a CI run (requires a push):** The Windows build has never compiled. Biggest unknown is
**Bungee under MSVC** (the plan flagged this) — it may need flag/intrinsic tweaks. The Bungee `git apply` patch
step needs Git on the runner (present on `windows-latest`). chromaprint+kissfft and JUCE 8.0.12 under MSVC are
expected fine. If the Windows job goes red, start with Bungee compile errors.

**Optional / not yet done:** `API_KEYS_H` GitHub secret (build is green without it via the example fallback),
FetchContent/ONNX caching (Phase 4.5), `pluginval` step (6.4), Phase 5 (Windows installer/signing), Phase 6.2
(load `.vst3` in a real Windows DAW), Phase 7 (ongoing workflow). **Note:** an in-app **UpdateChecker** feature
(`UpdateChecker.cpp/.h`, `CUE_VERSION_STRING`) was added since the plan was written — unrelated to the port.

---

## 1. Project snapshot (verified facts)

- **Repo:** `https://github.com/cuevst-cmd/cuesampler.git`, default branch `main`.
- **Framework:** JUCE (C++20), CMake-based (`cmake_minimum_required 3.22`).
- **Plugin:** `CueSampler` — a synth/sampler. `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`.
  - Product name: `CUE SAMPLER`. Company: `CUE SOFTWARE`. Bundle ID `com.cuesoftware.cuesampler`.
  - Manufacturer code `Cues`, plugin code `Csam`.
- **Formats built:** `AU VST3 Standalone` (AU is macOS-only and will simply be skipped by JUCE on Windows).
- **Source files** (all top-level, all confirmed portable — see §2):
  `PluginProcessor.cpp/.h`, `PluginEditor.cpp/.h`, `AudioFingerprinter`, `EditTelemetry`,
  `UpdateChecker`, `KeyDetector`, `BeatThisAnalyzer`, `WarpMap`, `ChopAudioCache`,
  `SSLBusCompressor`, `BitCrusher`.
- **Runtime assets** (in `assets/`): `cue_logo.png` (embedded via `juce_add_binary_data`),
  `beat_this.onnx` (~2.2 MB), `beat_this.onnx.data` (~82 MB), `Syne-wght.ttf` font.

### Dependencies and how they are currently wired
| Dependency | How it's pulled today | Portable? | Notes |
|---|---|---|---|
| **JUCE** | `add_subdirectory(../JUCE ...)` — a **sibling folder**, not tracked in the repo, not a submodule | ❌ Not reproducible | Biggest hidden blocker. A clean checkout has no `../JUCE`. |
| **Bungee** (time-stretch) | `add_subdirectory()` from a **hardcoded absolute Mac path** `/Users/jerryvolpe/Documents/AutoChopSampler/cmake-build-debug/_deps/bungee-src` | ❌ Not reproducible | Path exists only on the author's Mac. |
| **chromaprint** (v1.5.1) | `FetchContent` from GitHub, `FFT_LIB=kissfft` forced | ✅ Yes | Good — kissfft avoids the Apple-only vDSP FFT path. Should build on Windows as-is. |
| **ONNX Runtime** (1.20.1) | Prebuilt **`.dylib`** in `libs/onnxruntime/lib/`, BUT `*.dylib` is **gitignored** | ❌ macOS-only AND not in repo | Needs Windows `.dll`+`.lib`; must be fetched in CI for both OSes. |
| **Eigen** | Header-only, via Bungee's `submodules/eigen` include path | ⚠️ Tied to Bungee | Resolves once Bungee is portable. |

### `.gitignore` consequences (critical)
`.gitignore` excludes `build/`, `cmake-build-*/`, `*.vst3`, `*.dll`, `*.dylib`, `*.so`, `*.exe`,
`*.component`, `JuceLibraryCode/`, `*.jucer`, and `APIKeys.h`. Implications:
- The ONNX `.dylib` files are **not committed** → CI must download ONNX Runtime per-platform.
- `APIKeys.h` is **not committed** → CI needs it injected as a secret or generated (see Phase 4).
- The `assets/*.onnx*` files are **not** ignored, so they're tracked — but `beat_this.onnx.data`
  is ~82 MB. Under GitHub's 100 MB hard limit but over the 50 MB warning threshold. Consider Git LFS
  (see Open Questions). **Confirm it is actually committed** before relying on CI finding it.

---

## 2. Portability assessment of the source code

**The C++ source is clean and portable.** Grep audit results:
- **No** Objective-C / Objective-C++ (`.mm`) files in the project's own code.
- **No** Mac-only frameworks (`Cocoa`, `AppKit`, `CoreAudio`, `Accelerate`, `AudioToolbox`, etc.)
  included anywhere in the author's source. (The only `Accelerate` reference is inside the
  chromaprint dependency's `fft_lib_vdsp.h`, which is **not compiled** because `kissfft` is forced.)
- File-path logic uses JUCE's cross-platform `File::getSpecialLocation(...)`
  (`currentExecutableFile`, `tempDirectory`, `userApplicationDataDirectory`) — all portable.

**One harmless cleanup in source:** `PluginProcessor.cpp` (~line 1911) has a hardcoded last-resort
dev path `"/Users/jerryvolpe/Documents/SAMPLERv3/assets/beat_this.onnx"`. It's a fallback that
simply won't match on Windows, so it won't break anything, but it should be removed or guarded.

**Conclusion:** No plugin logic needs rewriting. JUCE abstracts the OS. **All real work is in the
build system and dependency acquisition**, plus CI and packaging.

---

## 3. Strategy: make ONE source tree build on both OSes

Guiding principles:
1. **Reproducible dependencies first.** Every dependency must be fetchable on a clean machine with
   no manual steps. Prefer `FetchContent` (pinned by tag/commit) or git submodules over loose
   sibling folders and absolute paths.
2. **Platform branches in CMake, not in code.** Use `if(APPLE) / elseif(WIN32)` blocks for the
   bundling/signing differences. Keep the shared build identical.
3. **Never break the working Mac build.** Every change must keep the existing macOS output
   byte-for-byte equivalent in behavior. Validate Mac after each phase before touching Windows.
4. **CI is the Windows machine.** GitHub Actions `windows-latest` (MSVC) produces the `.vst3`.
   Add macOS to the same workflow as a build matrix so both stay green on every push.

---

## 4. Step-by-step plan (phased)

### Phase 0 — Repo hygiene & reproducibility prerequisites
- [ ] **0.1** Clone fresh into a scratch dir to confirm what's actually committed vs. local-only.
      Expect the build to FAIL (missing `../JUCE`, missing Bungee path, missing ONNX libs). Document
      the exact failures — they define the work.
- [ ] **0.2** Decide dependency-vendoring strategy (recommendation in Phase 1). Write it down.
- [ ] **0.3** Confirm `beat_this.onnx.data` (~82 MB) is committed and pulls cleanly. If not, set up
      **Git LFS** for `assets/*.onnx*` before doing anything else.
- [ ] **0.4** Confirm how `APIKeys.h` is meant to be provided in CI (it's gitignored). Inspect
      `APIKeys.h.example` to see required symbols. Plan a CI step to generate it from GitHub Secrets.

### Phase 1 — Make every dependency portable & pinned
- [ ] **1.1 JUCE.** Convert the `../JUCE` sibling into either:
      - **(Recommended)** a pinned `FetchContent_Declare(JUCE GIT_REPOSITORY ... GIT_TAG <version>)`, or
      - a git **submodule** at `./JUCE` (then `add_subdirectory(JUCE)`).
      First, determine the **exact JUCE version** currently used locally (check `../JUCE/CMakeLists.txt`
      project version, or `git -C ../JUCE describe --tags`) and pin to it so behavior doesn't shift.
- [ ] **1.2 Bungee.** Replace the absolute-path `add_subdirectory` with a pinned `FetchContent` or
      submodule. **Action:** identify the exact Bungee source currently in use
      (`/Users/jerryvolpe/Documents/AutoChopSampler/cmake-build-debug/_deps/bungee-src`) — find its
      git remote/commit (`git -C <that path> remote -v && git -C <that path> rev-parse HEAD`) and pin
      that. Bungee bundles **Eigen** as a submodule, so ensure submodules are fetched
      (`FetchContent` with `GIT_SUBMODULES`, or recursive submodule init). Verify Bungee builds with
      MSVC — confirm its CMake doesn't assume Clang/GCC-only flags.
- [ ] **1.3 ONNX Runtime 1.20.1.** Do NOT commit binaries. Instead, in CMake/CI, download the
      official prebuilt per-platform package:
      - macOS: `onnxruntime-osx-universal2-1.20.1` (universal2 covers arm64 + x86_64).
      - Windows: `onnxruntime-win-x64-1.20.1` (provides `onnxruntime.dll`, `onnxruntime.lib`, headers).
      Wire a CMake variable like `ONNXRUNTIME_ROOT` that points at the unpacked package, and link
      `${ONNXRUNTIME_ROOT}/lib/<platform lib>`. Keep the existing local `libs/onnxruntime/` as a
      fallback for the author's Mac if desired, but CI should use the downloaded copy.
- [ ] **1.4 chromaprint.** No change needed; it already uses `FetchContent` + `kissfft`. Just verify
      it configures under MSVC (it does in general, but confirm in CI).

### Phase 2 — Cross-platform CMakeLists.txt refactor
> Keep all existing macOS behavior inside `if(APPLE)`. Add `elseif(WIN32)` equivalents. See Appendix A
> for concrete proposed snippets.
- [ ] **2.1** Wrap the macOS-only block (`CMAKE_OSX_ARCHITECTURES`, `CMAKE_OSX_DEPLOYMENT_TARGET`) —
      already guarded by `if(APPLE)`. Leave as-is.
- [ ] **2.2** Replace the hardcoded `libonnxruntime.dylib` link (line ~152) with a platform-selected
      ONNX import target / library path driven by `ONNXRUNTIME_ROOT` from Phase 1.3.
- [ ] **2.3** Split the **POST_BUILD bundling** logic (lines ~165–235), which is entirely macOS
      (`install_name_tool`, `codesign`, `@loader_path`, `@rpath`, `Contents/Resources`):
      - `if(APPLE)`: keep the current dylib-patch + re-sign + Resources copy.
      - `elseif(WIN32)`: just **copy `onnxruntime.dll`** next to the built `.vst3` binary
        (`$<TARGET_FILE_DIR:CueSampler_VST3>`), and copy `beat_this.onnx` + `beat_this.onnx.data`
        into the same folder or a known relative location. No signing/patching needed at build time.
- [ ] **2.4** Confirm the ONNX model-file discovery in `PluginProcessor.cpp` (the `findOnnxModel`
      lambda) has a path that works for the Windows layout chosen in 2.3. On macOS it looks in
      `../Resources/`; on Windows there is no `Contents/Resources`, so ensure one of the search
      fallbacks (next-to-binary) matches where 2.3 copies the model. Adjust the lambda if needed.
- [ ] **2.5** Verify `juce_add_binary_data` (logo) and the Syne font are loaded portably (they are —
      embedded/BinaryData), no change expected.

### Phase 3 — Source code fixes
- [ ] **3.1** Remove/guard the hardcoded macOS dev path in `PluginProcessor.cpp` (~line 1911).
- [ ] **3.2** Audit `UpdateChecker` (it talks to GitHub Releases) and `EditTelemetry` for any
      assumptions about path separators or shell calls — quick check; JUCE `File` handles separators,
      so likely clean. Confirm no `popen`/`system()` shell-outs (none found in the audit).
- [ ] **3.3** Build-flag sanity: project uses `JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0` — fine on
      Windows. `juce_recommended_warning_flags` + LTO: confirm LTO is acceptable under MSVC in CI
      (it is, but watch build time).

### Phase 4 — GitHub Actions CI (Windows + macOS matrix)
> Full starter workflow in Appendix B. Primary deliverable for the "build on Windows without a
> Windows machine" requirement.
- [ ] **4.1** Add `.github/workflows/build.yml` with a matrix over `windows-latest` and
      `macos-latest`.
- [ ] **4.2** Steps per OS: checkout (with submodules if used) → install CMake/Ninja → download &
      unpack the correct ONNX Runtime package → generate `APIKeys.h` from secrets →
      `cmake -B build -DONNXRUNTIME_ROOT=...` → `cmake --build build --config Release`.
- [ ] **4.3** Upload the built artifacts (`.vst3`, Windows `.dll`+model files, macOS `.vst3`/`.component`)
      via `actions/upload-artifact` so each run produces downloadable plugins.
- [ ] **4.4** Add `APIKeys.h` contents as repo secrets; CI step writes the file before configure.
- [ ] **4.5** (Optional) Cache `FetchContent` deps and the ONNX download to speed up runs.

### Phase 5 — Windows packaging & signing (defer until a green build exists)
- [ ] **5.1** Windows install layout: VST3 goes to `C:\Program Files\Common Files\VST3\`. Decide
      whether to ship a folder-style `.vst3` bundle (recommended; mirrors macOS) containing the binary
      + `onnxruntime.dll` + model files under `Contents/x86_64-win/` and `Contents/Resources/`.
- [ ] **5.2** Build a Windows installer (Inno Setup or WiX). Mirror the Mac `make-installer.sh` flow.
- [ ] **5.3** Code signing: Windows Authenticode cert (separate from Apple notarization). Plan as a
      later CI step using a secret-stored cert. The Mac scripts (`notarize.sh`, `staple-installer.sh`,
      `codesign`) stay macOS-only.

### Phase 6 — Testing & verification
- [ ] **6.1** CI build green on both OSes (compiles + links + produces artifacts).
- [ ] **6.2** **Functional test on real Windows** — CI proves it compiles, NOT that it runs. Load the
      `.vst3` in a Windows DAW (Reaper trial / FL Studio demo) on a cheap cloud Windows instance or VM.
      Verify: plugin scans, UI renders (font + logo), audio + MIDI work, ONNX beat analysis loads
      (check the log line "BeatThisAnalyzer: ready=YES"), and the autocorrelation fallback triggers
      gracefully if the model is missing.
- [ ] **6.3** Re-verify the macOS build is unchanged after all refactors (regression check).
- [ ] **6.4** Add `pluginval` (JUCE's validation tool) as a CI step on both OSes for automated sanity.

### Phase 7 — Ongoing dual-platform workflow (the long-term answer)
- Develop on Mac as usual; every `git push` triggers CI that builds & validates **both** OSes.
- Treat a red Windows build like a red Mac build — fix before merging.
- Keep dependency versions pinned; bump JUCE/ONNX/Bungee deliberately, not implicitly.
- For Windows-specific runtime testing, keep a cheap on-demand cloud Windows box; you don't need it
  for compiling, only for verifying behavior before releases.

---

## 5. Acceptance criteria (definition of done for the next session)
1. A clean `git clone` builds on macOS with **no manual dependency setup** (deps fetched by CMake/CI).
2. The same clean clone builds a working `.vst3` on `windows-latest` via GitHub Actions.
3. GitHub Actions runs a Windows+macOS matrix on every push and uploads plugin artifacts.
4. The existing macOS plugin behavior is unchanged (regression-verified).
5. The Windows `.vst3` has been loaded and smoke-tested in at least one Windows DAW.

---

## 6. Open questions / things to confirm before/while executing
1. **Exact JUCE version** in `../JUCE` — must be pinned (run `git -C ../JUCE describe --tags` or read
   its CMake project version). The build's behavior depends on it.
2. **Exact Bungee source + commit** — get the remote and SHA from the local checkout and confirm it
   builds with MSVC. Does Bungee's CMake support Windows out of the box? Confirm Eigen submodule pulls.
3. **`beat_this.onnx.data` (82 MB)** — is it committed? Should it move to **Git LFS**? Or be downloaded
   at build time from a release asset to keep the repo lean? Decide before CI relies on it.
4. **`APIKeys.h`** — what symbols does it define (see `APIKeys.h.example`)? Which belong in GitHub
   Secrets? Confirm none are required at build time that can't be safely injected.
5. **ONNX model layout on Windows** — confirm `findOnnxModel()` in `PluginProcessor.cpp` will locate
   the model given the chosen Windows file layout (Phase 2.3/2.4).
6. **VST2 / AAX?** Currently only AU/VST3/Standalone. Confirm no plan to add VST2 (needs the legacy
   SDK) or AAX (needs the Avid SDK + signing) — out of scope unless requested.
7. **Minimum Windows version** target (Windows 10 x64 assumed). Confirm.

---

## Appendix A — Proposed CMakeLists.txt changes (drafts to verify, NOT yet applied)

> These are starting points. The next session must verify exact tag/commit values and test them.

**A.1 — Pin JUCE via FetchContent (replaces `add_subdirectory(../JUCE ...)`):**
```cmake
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG <PIN_TO_CURRENT_LOCAL_VERSION>)   # e.g. 8.0.x — confirm from ../JUCE
FetchContent_MakeAvailable(JUCE)
```

**A.2 — Pin Bungee via FetchContent (replaces the absolute-path add_subdirectory):**
```cmake
FetchContent_Declare(bungee
    GIT_REPOSITORY <BUNGEE_REMOTE_URL>        # get from local checkout's `git remote -v`
    GIT_TAG <BUNGEE_COMMIT_SHA>
    GIT_SUBMODULES_RECURSE TRUE)              # pulls Eigen
set(BUNGEE_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(bungee)
# update target_include_directories to use ${bungee_SOURCE_DIR} instead of the hardcoded path
```

**A.3 — Platform-selected ONNX Runtime (replaces hardcoded .dylib):**
```cmake
# ONNXRUNTIME_ROOT points at an unpacked official prebuilt package (set by CI or locally)
if(APPLE)
    set(ORT_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.1.20.1.dylib")
elseif(WIN32)
    set(ORT_LIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
endif()
target_include_directories(CueSampler PRIVATE "${ONNXRUNTIME_ROOT}/include")
target_link_libraries(CueSampler PRIVATE "${ORT_LIB}")
```

**A.4 — Platform-split POST_BUILD (replaces the macOS-only bundling block):**
```cmake
if(APPLE)
    # ... keep existing install_name_tool + codesign + Resources copy unchanged ...
elseif(WIN32)
    add_custom_command(TARGET CueSampler_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:CueSampler_VST3>/onnxruntime.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx"
            "$<TARGET_FILE_DIR:CueSampler_VST3>/beat_this.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx.data"
            "$<TARGET_FILE_DIR:CueSampler_VST3>/beat_this.onnx.data"
        COMMENT "Windows: stage onnxruntime.dll + model next to plugin")
endif()
```
> Note: confirm `findOnnxModel()` checks "next to the binary," which it does (fallback #2), so the
> Windows layout above will be found.

---

## Appendix B — Proposed GitHub Actions workflow (`.github/workflows/build.yml`, draft)

```yaml
name: Build CUE Sampler
on:
  push: { branches: [ main ] }
  pull_request:
  workflow_dispatch:

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: windows-latest
            ort_url: https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip
          - os: macos-latest
            ort_url: https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-universal2-1.20.1.tgz
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive      # if JUCE/Bungee become submodules; harmless otherwise

      - name: Install build tools
        uses: lukka/get-cmake@latest

      - name: Fetch ONNX Runtime (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          Invoke-WebRequest "${{ matrix.ort_url }}" -OutFile ort.zip
          Expand-Archive ort.zip -DestinationPath ort
          "ONNXRUNTIME_ROOT=$((Get-ChildItem ort -Directory | Select-Object -First 1).FullName)" >> $env:GITHUB_ENV

      - name: Fetch ONNX Runtime (macOS)
        if: runner.os == 'macOS'
        run: |
          curl -L "${{ matrix.ort_url }}" -o ort.tgz
          mkdir ort && tar -xzf ort.tgz -C ort
          echo "ONNXRUNTIME_ROOT=$(pwd)/ort/$(ls ort)" >> "$GITHUB_ENV"

      - name: Generate APIKeys.h from secret
        shell: bash
        run: printf '%s' "${{ secrets.API_KEYS_H }}" > APIKeys.h

      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_ROOT="${{ env.ONNXRUNTIME_ROOT }}"

      - name: Build
        run: cmake --build build --config Release --parallel

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: cue-sampler-${{ matrix.os }}
          path: |
            build/**/*.vst3
            build/**/*.component
            build/**/onnxruntime.dll
```
> Notes: secret `API_KEYS_H` holds the full contents of `APIKeys.h`. If JUCE/Bungee stay as
> `FetchContent` (not submodules), `submodules: recursive` is a harmless no-op. Add caching and a
> `pluginval` step once the basic build is green.

---

## Appendix C — Quick command reference for the next session
```bash
# Confirm pinned versions to use:
git -C ../JUCE describe --tags                 # JUCE version to pin
git -C /Users/jerryvolpe/Documents/AutoChopSampler/cmake-build-debug/_deps/bungee-src remote -v
git -C /Users/jerryvolpe/Documents/AutoChopSampler/cmake-build-debug/_deps/bungee-src rev-parse HEAD
# Check whether the big model file is tracked:
git ls-files --error-unmatch assets/beat_this.onnx.data && echo committed || echo NOT committed
# See what APIKeys.h must define:
cat APIKeys.h.example
```
