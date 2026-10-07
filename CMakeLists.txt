cmake_minimum_required(VERSION 3.22)
project(Vivisect VERSION 0.9.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_TAG "8.0.4" CACHE STRING "JUCE git tag")
option(VIVISECT_SYSTEM_JUCE "Use find_package(JUCE) instead of fetching" OFF)

# CLAP also uses FetchContent below, so load the module regardless of whether
# JUCE itself comes from find_package() or from the network.
include(FetchContent)

if(VIVISECT_SYSTEM_JUCE)
    find_package(JUCE CONFIG REQUIRED)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${JUCE_TAG}
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
#  Formats. VST3 and Standalone everywhere; LV2 additionally on Linux, where
#  it is the format most hosts (Ardour, Qtractor, Zrythm) reach for first.
#  CLAP is added separately below, because JUCE has no native CLAP wrapper.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    set(VIVISECT_FORMATS VST3 LV2 Standalone CACHE STRING "Plugin formats to build")
else()
    set(VIVISECT_FORMATS VST3 Standalone CACHE STRING "Plugin formats to build")
endif()

juce_add_plugin(Vivisect
    COMPANY_NAME "Specimen Audio"
    BUNDLE_ID com.specimenaudio.vivisect
    PLUGIN_MANUFACTURER_CODE Spcm
    PLUGIN_CODE Vvsc
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS ${VIVISECT_FORMATS}
    PRODUCT_NAME "Vivisect"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon_small.png"
    VST3_CATEGORIES "Fx" "Distortion"
    LV2URI "https://specimenaudio.com/plugins/vivisect")

target_sources(Vivisect PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Dsp.cpp
    Source/Theme.cpp
    Source/Ui.cpp)

target_include_directories(Vivisect PRIVATE Source)

target_compile_definitions(Vivisect PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

juce_generate_juce_header(Vivisect)

target_link_libraries(Vivisect PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
#  CLAP
#
#  JUCE ships no CLAP wrapper, so this uses clap-juce-extensions, which wraps
#  the existing Vivisect shared-code target. The CLAP is therefore the same
#  plugin as the VST3 - same DSP, same editor, same state - rather than a
#  parallel implementation that could drift.
#
#  Turn off with -DVIVISECT_BUILD_CLAP=OFF (it needs network on first config).
# ---------------------------------------------------------------------------
option(VIVISECT_BUILD_CLAP "Build the CLAP version" ON)

if(VIVISECT_BUILD_CLAP)
    FetchContent_Declare(clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        # Pinned so a release build is reproducible; bump deliberately.
        GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d)
    FetchContent_MakeAvailable(clap-juce-extensions)

    clap_juce_extensions_plugin(TARGET Vivisect
        CLAP_ID "com.specimenaudio.vivisect"
        CLAP_FEATURES audio-effect distortion glitch stereo
        CLAP_MANUAL_URL "https://specimenaudio.com/vivisect"
        CLAP_SUPPORT_URL "https://specimenaudio.com/support")
endif()

# ---------------------------------------------------------------------------
#  Headless test harness. include.md asks for every feature and every
#  combination of settings to be exercised; doing that by hand is not
#  repeatable, so the checks are a build target.
#
#  This links the plugin's own SharedCode library rather than recompiling the
#  sources. That means the code under test is the exact object code the plugin
#  ships, and it keeps the JUCE modules from being built a second time - which
#  matters on a memory-constrained machine.
#
#    cmake --build build --config Release --target VivisectTests
#    build/Release/VivisectTests.exe
# ---------------------------------------------------------------------------
option(VIVISECT_BUILD_TESTS "Build the headless test harness" ON)

if(VIVISECT_BUILD_TESTS)
    add_executable(VivisectTests Tests/TestMain.cpp)

    # juce_generate_juce_header(Vivisect) writes JuceHeader.h into the plugin's
    # artefacts dir and keeps that include path private to the plugin target, so
    # the harness has to be pointed at it explicitly.
    target_include_directories(VivisectTests PRIVATE
        Source
        ${CMAKE_CURRENT_BINARY_DIR}/Vivisect_artefacts/JuceLibraryCode
        ${juce_SOURCE_DIR}/modules)

    # Vivisect is the plugin's SharedCode target: it carries the compiled JUCE
    # modules, the generated JuceHeader.h include path and the JucePlugin_*
    # defines, so the harness inherits the shipping configuration wholesale.
    target_link_libraries(VivisectTests PRIVATE
        Vivisect
        juce::juce_recommended_config_flags)

    target_compile_definitions(VivisectTests PRIVATE
        JUCE_STANDALONE_APPLICATION=1)

    # A console program, so its report reaches the terminal / CI log.
    if(MSVC)
        target_link_options(VivisectTests PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
endif()
