

cmake_minimum_required(VERSION 3.22)

# The top-level CMakeLists.txt file for a project must contain a literal, direct call to the
# `project()` command. `project()` sets up some helpful variables that describe source/binary
# directories, and the current project version. This is a standard CMake command.

project(CUESAMPLER VERSION 0.0.1)
include(FetchContent)

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

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")

set(BUNGEE_SOURCE_DIR "/Users/jerryvolpe/Documents/AutoChopSampler/cmake-build-debug/_deps/bungee-src")
if (NOT EXISTS "${BUNGEE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Local Bungee source tree not found at ${BUNGEE_SOURCE_DIR}")
endif()

set(BUNGEE_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${BUNGEE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bungee")

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
    COPY_PLUGIN_AFTER_BUILD TRUE                # Install the plugin to the default location after each build
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
        KeyDetector.cpp
        PluginEditor.cpp
        PluginProcessor.cpp
        BeatThisAnalyzer.cpp
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
        JUCE_VST3_CAN_REPLACE_VST2=0)

# If your target needs extra binary assets, you can add them here. The first argument is the name of
# a new static library target that will include all the binary resources. There is an optional
# `NAMESPACE` argument that can specify the namespace of the generated binary data class. Finally,
# the SOURCES argument should be followed by a list of source files that should be built into the
# static library. These source files can be of any kind (wav data, images, fonts, icons etc.).
# Conversion to binary-data will happen when your target is built.

juce_add_binary_data(CueSamplerBinaryAssets
    NAMESPACE CueSamplerBinaryData
    SOURCES
        assets/cue_logo.png)

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
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/onnxruntime/lib/libonnxruntime.dylib"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

target_include_directories(CueSampler
    PRIVATE
        "${BUNGEE_SOURCE_DIR}"
        "${BUNGEE_SOURCE_DIR}/submodules/eigen"
        "${chromaprint_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/onnxruntime/include")

# ── Self-contained VST3 bundle ────────────────────────────────────────────────
# Copies libonnxruntime into Contents/MacOS/ (so @loader_path resolves it) and
# ONNX model files into Contents/Resources/, then patches the binary's dylib
# reference to use @loader_path.  The resulting .vst3 can be zipped and sent to
# beta testers with no external dependencies.
set(ONNX_DYLIB "${CMAKE_CURRENT_SOURCE_DIR}/libs/onnxruntime/lib/libonnxruntime.1.20.1.dylib")

add_custom_command(TARGET CueSampler_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:CueSampler_VST3>/../Resources"

    # Copy libonnxruntime next to the plugin binary
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNX_DYLIB}"
        "$<TARGET_FILE_DIR:CueSampler_VST3>/libonnxruntime.1.20.1.dylib"

    # Copy ONNX model files into Resources/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx"
        "$<TARGET_FILE_DIR:CueSampler_VST3>/../Resources/beat_this.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx.data"
        "$<TARGET_FILE_DIR:CueSampler_VST3>/../Resources/beat_this.onnx.data"

    # Patch the plugin binary so it finds libonnxruntime via @loader_path
    COMMAND install_name_tool
        -change "@rpath/libonnxruntime.1.20.1.dylib"
                "@loader_path/libonnxruntime.1.20.1.dylib"
        "$<TARGET_FILE:CueSampler_VST3>"

    # Re-sign both after modifications — install_name_tool invalidates signatures
    COMMAND codesign --force --sign -
        "$<TARGET_FILE_DIR:CueSampler_VST3>/libonnxruntime.1.20.1.dylib"
    COMMAND codesign --force --sign -
        "$<TARGET_FILE:CueSampler_VST3>"

    COMMENT "Bundling libonnxruntime + ONNX model into VST3 and re-signing"
)

# ── Self-contained AU bundle ──────────────────────────────────────────────────
add_custom_command(TARGET CueSampler_AU POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:CueSampler_AU>/../Resources"

    # Copy libonnxruntime next to the AU binary
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNX_DYLIB}"
        "$<TARGET_FILE_DIR:CueSampler_AU>/libonnxruntime.1.20.1.dylib"

    # Copy ONNX model files into Resources/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx"
        "$<TARGET_FILE_DIR:CueSampler_AU>/../Resources/beat_this.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/beat_this.onnx.data"
        "$<TARGET_FILE_DIR:CueSampler_AU>/../Resources/beat_this.onnx.data"

    # Patch the AU binary so it finds libonnxruntime via @loader_path
    COMMAND install_name_tool
        -change "@rpath/libonnxruntime.1.20.1.dylib"
                "@loader_path/libonnxruntime.1.20.1.dylib"
        "$<TARGET_FILE:CueSampler_AU>"

    # Re-sign both after modifications
    COMMAND codesign --force --sign -
        "$<TARGET_FILE_DIR:CueSampler_AU>/libonnxruntime.1.20.1.dylib"
    COMMAND codesign --force --sign -
        "$<TARGET_FILE:CueSampler_AU>"

    COMMENT "Bundling libonnxruntime + ONNX model into AU and re-signing"
)
