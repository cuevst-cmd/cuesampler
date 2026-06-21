

cmake_minimum_required(VERSION 3.22)

# The top-level CMakeLists.txt file for a project must contain a literal, direct call to the
# `project()` command. `project()` sets up some helpful variables that describe source/binary
# directories, and the current project version. This is a standard CMake command.

project(CUESAMPLER VERSION 1.0.1)
include(FetchContent)

# Copy the built plugin into the OS plugin directory after each build. On by
# default for local dev; CI sets -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF so it never
# tries to write to a system location (e.g. Program Files) on the runner.
option(CUE_COPY_PLUGIN_AFTER_BUILD "Install plugin to the default OS plugin dir after build" ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build for both architectures" FORCE)
    # Minimum supported macOS. Without this, the binary inherits the build
    # machine's SDK version as its floor (macOS 26), which makes the plugin
    # refuse to load on older systems. macOS 15.0 is our supported floor; the
    # bundled onnxruntime (minos 13.3) is satisfied by this.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFT_LIB kissfft CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(KISSFFT_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/chromaprint-src/src/3rdparty/kissfft" CACHE PATH "" FORCE)
FetchContent_Declare(chromaprint
    GIT_REPOSITORY https://github.com/acoustid/chromaprint.git
    GIT_TAG v1.5.1)
FetchContent_MakeAvailable(chromaprint)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# If you've installed JUCE somehow (via a package manager, or directly using the CMake install
# target), you'll need to tell this project that it depends on the installed copy of JUCE. If you've
# included JUCE directly in your source tree (perhaps as a submodule), you'll need to tell CMake to
# include that subdirectory as part of the build.

# JUCE — pinned to the version used locally (8.0.12). Fetched on a clean
# checkout / CI so no sibling ../JUCE folder is required.
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Bungee (time-stretch) — pinned to the exact commit previously used locally.
# Bungee vendors Eigen as a git submodule under submodules/eigen, which
# FetchContent initialises by default. No GIT_SHALLOW here: shallow clones
# can't reliably fetch an arbitrary commit SHA.
#
# PATCH_COMMAND applies our local Stream::reset() addition (see
# patches/bungee-stream-reset.patch) on top of pristine upstream. The plugin
# depends on it; keeping it as a patch avoids maintaining a Bungee fork.
set(BUNGEE_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(bungee
    GIT_REPOSITORY https://github.com/bungee-audio-stretch/bungee.git
    GIT_TAG 7354c0c62652dd85af90fddfeec307881f3b4252
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=<SOURCE_DIR>
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/bungee-stream-reset.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_bungee_patch.cmake)
FetchContent_MakeAvailable(bungee)

# ── Windows/MSVC: make pristine Bungee + pffft compile under cl.exe ────────────
# Upstream Bungee assumes a GCC/Clang + POSIX toolchain in three spots the MSVC
# build trips over:
#   * src/Assert.cpp           — #include <unistd.h>     (POSIX-only; used only
#                                by the disabled BUNGEE_PETRIFY path)
#   * src/Resample.h           — __attribute__((noinline)) (GCC/Clang attribute)
#   * submodules/pffft/pffft.c — __builtin_prefetch()    (GCC/Clang builtin, used
#                                outside pffft's own MSVC macro path)
# Rather than fork Bungee, we put an empty <unistd.h> stub on the include path
# and force-include a shim that no-ops __attribute__ and __builtin_prefetch
# (both are hints — dropping them is correctness-neutral). Applied only to the
# Bungee targets, only on MSVC, so macOS/Linux are untouched. Keeps us pinned to
# pristine upstream (plus the stream-reset patch).
if(WIN32 AND MSVC)
    set(_bungee_win_compat "${CMAKE_CURRENT_SOURCE_DIR}/cmake/win-compat")
    foreach(_btgt bungee_library pffft)
        if(TARGET ${_btgt})
            target_include_directories(${_btgt} PRIVATE "${_bungee_win_compat}")
            target_compile_options(${_btgt} PRIVATE "/FI${_bungee_win_compat}/bungee_msvc_shim.h")
        endif()
    endforeach()
    if(TARGET bungee_executable)
        set_target_properties(bungee_executable PROPERTIES OUTPUT_NAME "bungee_cli")
    endif()
endif()

# ONNX Runtime 1.20.1 — official prebuilt package, downloaded per-platform.
# Not committed to the repo (binaries are gitignored). The archive has no
# CMakeLists.txt, so FetchContent_MakeAvailable just populates it.
#
# macOS uses the GitHub CPU build (CoreML is off by default — see StemSeparator).
# Windows uses the DirectML-enabled NuGet build instead of the GitHub CPU zip so
# the htdemucs stem pass can run on any DX12 GPU (NVIDIA/AMD/Intel), ~10x faster
# than the CPU path. The NuGet packages are zips with a different internal layout
# (headers under build/native/include, lib+dll under runtimes/win-x64/native);
# DOWNLOAD_NAME gives each .nupkg a .zip extension so FetchContent extracts it.
# onnxruntime 1.20.1's DML EP needs the matching DirectML 1.15.2 redistributable,
# shipped as its own NuGet (Microsoft.AI.DirectML) — staged next to the DLL below.
if(APPLE)
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-universal2-1.20.1.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(onnxruntime)
elseif(WIN32)
    FetchContent_Declare(onnxruntime
        URL https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1
        DOWNLOAD_NAME onnxruntime-directml-1.20.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(directml
        URL https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.2
        DOWNLOAD_NAME directml-1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(onnxruntime directml)
endif()

set(ONNXRUNTIME_ROOT "${onnxruntime_SOURCE_DIR}")
if(APPLE)
    set(ORT_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
    set(ORT_LINK_LIB    "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.1.20.1.dylib")
elseif(WIN32)
    set(ORT_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/build/native/include")
    set(ORT_LINK_LIB    "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.lib")
    set(ORT_RUNTIME_DLL "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.dll")
    set(DIRECTML_DLL    "${directml_SOURCE_DIR}/bin/x64-win/DirectML.dll")
endif()

# If you are building a VST2 or AAX plugin, CMake needs to be told where to find these SDKs on your
# system. This setup should be done before calling `juce_add_plugin`.

# juce_set_vst2_sdk_path(...)
# juce_set_aax_sdk_path(...)

# `juce_add_plugin` adds a static library target with the name passed as the first argument
# (AudioPluginExample here). This target is a normal CMake target, but has a lot of extra properties set
# up by default. As well as this shared code static library, this function adds targets for each of
# the formats specified by the FORMATS arguments. This function accepts many optional arguments.
# Check the readme at `docs/CMake API.md` in the JUCE repo for the full list.

juce_add_plugin(CueSampler
    # VERSION ...                               # Set this if the plugin version is different to the project version
    # ICON_BIG ...                              # ICON_* arguments specify a path to an image file to use as an icon for the Standalone
    # ICON_SMALL ...
    # COMPANY_NAME ...                          # Specify the name of the plugin's author
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    # NEEDS_MIDI_OUTPUT TRUE/FALSE              # Does the plugin need midi output?
    # IS_MIDI_EFFECT TRUE/FALSE                 # Is this plugin a MIDI effect?
    # EDITOR_WANTS_KEYBOARD_FOCUS TRUE/FALSE    # Does the editor need keyboard focus?
    COPY_PLUGIN_AFTER_BUILD ${CUE_COPY_PLUGIN_AFTER_BUILD}  # Install to the default plugin dir after each build (off in CI)
    COMPANY_NAME "CUE SOFTWARE"
    BUNDLE_ID "com.cuesoftware.cuesampler"
    PLUGIN_MANUFACTURER_CODE Cues               # A four-character manufacturer id with at least one upper-case character
    PLUGIN_CODE Csam                            # A unique four-character plugin id with exactly one upper-case character
                                                # GarageBand 10.3 requires the first letter to be upper-case, and the remaining letters to be lower-case
    FORMATS AU VST3 Standalone                  # The formats to build. Other valid formats are: AAX Unity VST AU AUv3
    PRODUCT_NAME "CUE SAMPLER")                 # The name of the final executable, which can differ from the target name

# `juce_generate_juce_header` will create a JuceHeader.h for a given target, which will be generated
# into your build tree. This should be included with `#include <JuceHeader.h>`. The include path for
# this header will be automatically added to the target. The main function of the JuceHeader is to
# include all your JUCE module headers; if you're happy to include module headers directly, you
# probably don't need to call this.

# juce_generate_juce_header(AudioPluginExample)

# `target_sources` adds source files to a target. We pass the target that needs the sources as the
# first argument, then a visibility parameter for the sources which should normally be PRIVATE.
# Finally, we supply a list of source files that will be built into the target. This is a standard
# CMake command.

target_sources(CueSampler
    PRIVATE
        AudioFingerprinter.cpp
        EditTelemetry.cpp
        UpdateChecker.cpp
        KeyDetector.cpp
        PluginEditor.cpp
        PluginProcessor.cpp
        BeatThisAnalyzer.cpp
        StemSeparator.cpp
        WarpMap.cpp
        ChopAudioCache.cpp
        SSLBusCompressor.cpp
        BitCrusher.cpp)

# `target_compile_definitions` adds some preprocessor definitions to our target. In a Projucer
# project, these might be passed in the 'Preprocessor Definitions' field. JUCE modules also make use
# of compile definitions to switch certain features on/off, so if there's a particular feature you
# need that's not on by default, check the module header for the correct flag to set here. These
# definitions will be visible both to your code, and also the JUCE module code, so for new
# definitions, pick unique names that are unlikely to collide! This is a standard CMake command.

target_compile_definitions(CueSampler
    PUBLIC
        # JUCE_WEB_BROWSER and JUCE_USE_CURL would be on by default, but you might not need them.
        JUCE_WEB_BROWSER=0  # If you remove this, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_plugin` call
        JUCE_USE_CURL=0     # If you remove this, add `NEEDS_CURL TRUE` to the `juce_add_plugin` call
        JUCE_VST3_CAN_REPLACE_VST2=0
        # Running version for the in-app update checker (compared against the
        # newest GitHub Release). Single source of truth: project(... VERSION).
        CUE_VERSION_STRING="${PROJECT_VERSION}")

# chromaprint.h declares its API as __declspec(dllimport) on Windows unless
# CHROMAPRINT_NODLL is defined. We link chromaprint as a STATIC library, so
# without this the linker looks for __imp_chromaprint_* (DLL-import symbols)
# that the static .lib doesn't contain → LNK2019. Define it on Windows only;
# the non-Windows header path uses visibility attributes and is unaffected.
if(WIN32)
    target_compile_definitions(CueSampler PRIVATE CHROMAPRINT_NODLL)
endif()

# If your target needs extra binary assets, you can add them here. The first argument is the name of
# a new static library target that will include all the binary resources. There is an optional
# `NAMESPACE` argument that can specify the namespace of the generated binary data class. Finally,
# the SOURCES argument should be followed by a list of source files that should be built into the
# static library. These source files can be of any kind (wav data, images, fonts, icons etc.).
# Conversion to binary-data will happen when your target is built.

juce_add_binary_data(CueSamplerBinaryAssets
    NAMESPACE CueSamplerBinaryData
    SOURCES
        assets/cue_logo.png
        assets/cue_background.png
        assets/cue_background_blur.png
        assets/Syne-wght.ttf)

# `target_link_libraries` links libraries and JUCE modules to other libraries or executables. Here,
# we're linking our executable target to the `juce::juce_audio_utils` module. Inter-module
# dependencies are resolved automatically, so `juce_core`, `juce_events` and so on will also be
# linked automatically. If we'd generated a binary data target above, we would need to link to it
# here too. This is a standard CMake command.

target_link_libraries(CueSampler
    PRIVATE
        CueSamplerBinaryAssets
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        chromaprint
        bungee_library
        "${ORT_LINK_LIB}"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

target_include_directories(CueSampler
    PRIVATE
        "${bungee_SOURCE_DIR}"
        "${bungee_SOURCE_DIR}/submodules/eigen"
        "${chromaprint_SOURCE_DIR}/src"
        "${ORT_INCLUDE_DIR}")

# ── Self-contained macOS/Windows bundles (libonnxruntime + ONNX models) ───────
# macOS: copy libonnxruntime into Contents/MacOS/ (so @loader_path resolves it)
# and the ONNX models into Contents/Resources/, patch the binary's dylib
# reference to @loader_path, then re-sign.  The resulting .vst3 / .component /
# .app is self-contained and can be zipped for beta testers.
#
# ORDERING IS LOAD-BEARING — do not split copies out of this single POST_BUILD:
#   * Every copied file (libonnxruntime, beat_this, AND the htdemucs stem
#     model) must land BEFORE the codesign step. The final codesign signs the
#     whole BUNDLE, which regenerates Contents/_CodeSignature/CodeResources to
#     seal Resources/. A file added after signing makes `codesign --verify`
#     report "a sealed resource is missing or invalid" → auval / notarization
#     fail. (Signing only the inner Mach-O binary does NOT reseal Resources.)
#   * This block runs BEFORE JUCE's COPY_PLUGIN_AFTER_BUILD install copy, so the
#     installed plugin (~/Library/Audio/Plug-Ins/...) gets the same complete,
#     validly-signed bundle. A separate later block would populate only the build
#     tree, leaving the DAW-loaded plugin without the stem models.
set(ONNX_DYLIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.1.20.1.dylib")

# Single base-htdemucs model. export_htdemucs.py writes ONE file,
# assets/htdemucs/htdemucs.onnx (~316 MB, opset 17, weights embedded — NO
# .onnx.data sidecar), that outputs all four stems in one pass. (This replaced
# the htdemucs_ft 3-specialist bundle, ~948 MB, which was ~3x slower.) It is far
# over GitHub's 100 MB limit, so it is gitignored and may be ABSENT on a clean
# checkout / CI; copy_if_exists.cmake silently skips it so the build still
# succeeds (StemSeparator falls back to the original buffer). NOT
# juce_add_binary_data — too large.
set(CUE_STEM_MODEL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/htdemucs/htdemucs.onnx")
set(CUE_COPY_IF_EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_exists.cmake")

if(APPLE)
    # VST3 (.vst3), AU (.component) and Standalone (.app) all share the bundle
    # layout: TARGET_FILE is Contents/MacOS/<bin>, so Resources is ../Resources
    # and the bundle root is ../.. relative to TARGET_FILE_DIR.
    foreach(_tgt CueSampler_VST3 CueSampler_AU CueSampler_Standalone)
        if(NOT TARGET ${_tgt})
            continue()
        endif()

        # Stem-model copy, built up here so it can be injected into the one
        # POST_BUILD command BEFORE codesign (copy_if_exists skips it if absent).
        set(_stem_copies
            COMMAND ${CMAKE_COMMAND}
                "-DSRC=${CUE_STEM_MODEL_SRC}"
                "-DDST=$<TARGET_FILE_DIR:${_tgt}>/../Resources/htdemucs/htdemucs.onnx"
                -P "${CUE_COPY_IF_EXISTS}")

        add_custom_command(TARGET ${_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${_tgt}>/../Resources"

            # libonnxruntime next to the binary (resolved via @loader_path)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNX_DYLIB}"
                "$<TARGET_FILE_DIR:${_tgt}>/libonnxruntime.1.20.1.dylib"

            # beat_this model into Resources/ (small, committed → direct copy)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx"
                "$<TARGET_FILE_DIR:${_tgt}>/../Resources/beat_this.onnx"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx.data"
                "$<TARGET_FILE_DIR:${_tgt}>/../Resources/beat_this.onnx.data"

            # htdemucs model into Resources/htdemucs/ — MUST precede codesign
            # (see ORDERING note above).
            ${_stem_copies}

            # Patch the binary so it finds libonnxruntime via @loader_path
            COMMAND install_name_tool
                -change "@rpath/libonnxruntime.1.20.1.dylib"
                        "@loader_path/libonnxruntime.1.20.1.dylib"
                "$<TARGET_FILE:${_tgt}>"

            # Re-sign LAST: nested dylib first, then the whole bundle so the seal
            # (CodeResources) covers every file copied above. install_name_tool
            # invalidated the binary's signature; the bundle sign re-signs it too.
            COMMAND codesign --force --sign -
                "$<TARGET_FILE_DIR:${_tgt}>/libonnxruntime.1.20.1.dylib"
            COMMAND codesign --force --sign -
                "$<TARGET_FILE_DIR:${_tgt}>/../.."

            COMMENT "Bundling libonnxruntime + ONNX + stem models into ${_tgt} and re-signing")
    endforeach()

elseif(WIN32)
    # Windows: no install_name/codesign step. Just stage onnxruntime.dll and the
    # ONNX model next to each runnable binary. findOnnxModel() in
    # PluginProcessor.cpp looks "next to the binary" (fallback #2), and Windows
    # resolves a DLL's dependencies from its own directory, so this is enough for
    # the .vst3 / Standalone to run self-contained.
    foreach(_fmt VST3 Standalone)
        if(TARGET CueSampler_${_fmt})
            # htdemucs model staged next to the binary (htdemucs/htdemucs.onnx),
            # in the SAME POST_BUILD so it precedes JUCE's install copy (so the
            # installed plugin gets it too). copy_if_exists skips it if absent.
            set(_stem_copies
                COMMAND ${CMAKE_COMMAND}
                    "-DSRC=${CUE_STEM_MODEL_SRC}"
                    "-DDST=$<TARGET_FILE_DIR:CueSampler_${_fmt}>/htdemucs/htdemucs.onnx"
                    -P "${CUE_COPY_IF_EXISTS}")
            add_custom_command(TARGET CueSampler_${_fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ORT_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:CueSampler_${_fmt}>/onnxruntime.dll"
                # DirectML.dll must sit next to onnxruntime.dll: the DML execution
                # provider loads it at session-create time, and Windows resolves a
                # DLL's dependencies from its own directory.
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIRECTML_DLL}"
                    "$<TARGET_FILE_DIR:CueSampler_${_fmt}>/DirectML.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx"
                    "$<TARGET_FILE_DIR:CueSampler_${_fmt}>/beat_this.onnx"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx.data"
                    "$<TARGET_FILE_DIR:CueSampler_${_fmt}>/beat_this.onnx.data"
                ${_stem_copies}
                COMMENT "Windows: staging onnxruntime.dll + DirectML.dll + ONNX + stem models next to CueSampler_${_fmt}")
        endif()
    endforeach()
endif() # APPLE / WIN32 — platform-specific bundling

# ── Optional offline StemSeparator test ───────────────────────────────────────
# OFF by default (mirrors tools/test_warp_map.cpp, which is unwired from the main
# target). Builds a small console app that runs StemSeparator on a WAV. Needs the
# htdemucs model present to actually pass; without it StemSeparator reports
# not-ready and the test exits cleanly with a skip notice.
#   cmake -S . -B build -DCUE_BUILD_STEM_TEST=ON
#   cmake --build build --target test_stem_separator
#   ./build/test_stem_separator_artefacts/test_stem_separator <song.wav> [model]
option(CUE_BUILD_STEM_TEST "Build the offline StemSeparator test" OFF)
if(CUE_BUILD_STEM_TEST)
    juce_add_console_app(test_stem_separator PRODUCT_NAME "test_stem_separator")
    # Keep it out of the default `all` build even when the option is ON — build it
    # only via `--target test_stem_separator`, so enabling the option never slows
    # or breaks a normal plugin build.
    set_target_properties(test_stem_separator PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(test_stem_separator PRIVATE
        tools/test_stem_separator.cpp
        StemSeparator.cpp)
    target_compile_definitions(test_stem_separator PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(test_stem_separator PRIVATE
        "${ORT_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_stem_separator PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_recommended_config_flags
        "${ORT_LINK_LIB}")
    if(APPLE)
        # So the test can find the bundled onnxruntime dylib when run in-tree.
        set_target_properties(test_stem_separator PROPERTIES
            BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib")
    endif()
endif()

if(WIN32 AND MSVC)
    foreach(_tgt CueSampler_VST3 CueSampler_Standalone)
        if(TARGET ${_tgt})
            target_link_libraries(${_tgt} PRIVATE delayimp.lib)
            target_link_options(${_tgt} PRIVATE "/DELAYLOAD:onnxruntime.dll")
        endif()
    endforeach()
endif()

