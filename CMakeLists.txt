cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
        "Minimum macOS deployment target")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING
        "macOS architectures")
endif()

project(SwaraXT VERSION 1.2.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    include(StandaloneMidiCapacity)
endif()
include(CompilerWarnings)

option(SWARA_BUILD_TESTS "Build SWARA XT test executables" OFF)

set(SWARAXT_SHRUTHI_ROOT "${CMAKE_SOURCE_DIR}/third_party/shruthi-1")
set(SWARAXT_SHRUTHI_ADAPTED_ROOT "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/adapted")
set(SWARAXT_INCLUDE_FIRST "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/include_first")
include(ShruthiRomResources)

# Classic oscillator implementation is an oracle/parity object only.
# Production compiles with SWARAXT_USE_AVRLIB_HD_VOICE=1 and renders through
# avrlib_hd::HdVoice, so oscillator.cc is unreachable in the shipping plugin.
set(SWARAXT_CLASSIC_OSCILLATOR_SOURCE
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/oscillator.cc")

set(SWARAXT_SHRUTHI_SOURCES
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice.cc"
    "${SWARAXT_SHRUTHI_RESOURCE_SOURCE}"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/part.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/patch.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/parameter.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/note_stack.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/clock.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/sequencer_settings.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/system_settings.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice_allocator.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/midi_dispatcher.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/random.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/string.cc"
)

set(SWARAXT_HOST_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/audio_out.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/display.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/editor.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/storage.cc"
)

set(SWARAXT_GENERATED_TUNING_HEADER
    "${CMAKE_BINARY_DIR}/generated/BuiltinTunings.h")
add_custom_command(
    OUTPUT "${SWARAXT_GENERATED_TUNING_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DCONVERTER=$<TARGET_FILE:SwaraXtTuningConverter>
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}/Source/Tuning/builtin
        -DOUTPUT=${SWARAXT_GENERATED_TUNING_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBuiltinTunings.cmake
    DEPENDS SwaraXtTuningConverter
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/12-edo.scl
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/19-edo.scl
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/24-edo.scl
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/31-edo.scl
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/just-major.scl
        ${CMAKE_SOURCE_DIR}/Source/Tuning/builtin/tritave.scl
        ${CMAKE_SOURCE_DIR}/cmake/EmbedBuiltinTunings.cmake
    VERBATIM
)
add_custom_target(SwaraXtBuiltinTunings DEPENDS "${SWARAXT_GENERATED_TUNING_HEADER}")

set(SWARAXT_ENGINE_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Tuning/TuningTable.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/PatchBridge.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/SwaraXtEngine.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/SampleRate/HostResampler.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/Filter/SwaraXtFilter.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardResources.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardFilter.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardEffects.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardProcessor.cpp"
)

set(SWARAXT_PLUGIN_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Plugin/PluginProcessor.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Plugin/PluginEditor.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Plugin/SwaraXtParameterLayout.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Plugin/ShruthiFactoryPresets.cpp"
)

set(SWARAXT_UI_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtLookAndFeel.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtPanels.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtDualSvfPanel.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtBoardPanel.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtSkin.cpp"
)

juce_add_binary_data(SwaraXTFonts
    SOURCES
        resources/Fonts/AdriaticaNext-Medium.ttf
        resources/Fonts/AdriaticaNext-CondensedBold.ttf
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/braid.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/montronedsp_wordmark.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/stones_letters.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/jam_ornament.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/midnight_backdrop.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/midnight_product_lockup.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/mod_matrix_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/visualizer_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_backdrop.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_braid.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_jam_ornament.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_mod_matrix_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_product_lockup.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_visualizer_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/vco1_header.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/vco2_header.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/pcb_trace_separator.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/greek_key_1turn.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/greek_key_4turns.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_plait.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_intertwined.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_guilloche.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_interlace.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_wicker.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_meander.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/roman_key_border.svg"
)

set(SWARAXT_ALL_PLUGIN_SOURCES
    ${SWARAXT_SHRUTHI_SOURCES}
    ${SWARAXT_HOST_SOURCES}
    ${SWARAXT_ENGINE_SOURCES}
    ${SWARAXT_PLUGIN_SOURCES}
    ${SWARAXT_UI_SOURCES}
)

function(swaraxt_configure_shruthi_target target)
    target_include_directories(${target}
        PRIVATE
            "${SWARAXT_INCLUDE_FIRST}"
            "${SWARAXT_SHRUTHI_ADAPTED_ROOT}"
            "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi"
            "${SWARAXT_SHRUTHI_ROOT}"
            "${SWARAXT_SHRUTHI_ROOT}/shruthi"
            "${SWARAXT_SHRUTHI_ROOT}/midi"
            "${CMAKE_SOURCE_DIR}/Source"
            "${CMAKE_BINARY_DIR}/generated"
    )
    target_compile_options(${target}
        PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/FI${SWARAXT_INCLUDE_FIRST}/swaraxt/shruthi_host.h>"
    )
    target_compile_definitions(${target}
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            SWARAXT_VERSION_STRING="${PROJECT_VERSION}"
    )
endfunction()

function(swaraxt_enable_avrlib_hd_voice target)
    target_compile_definitions(${target} PUBLIC SWARAXT_USE_AVRLIB_HD_VOICE=1)
endfunction()

option(SWARA_BUILD_STANDALONE "Build the standalone instrument" ON)
set(SWARAXT_PLUGIN_FORMATS VST3)
if(SWARA_BUILD_STANDALONE)
    list(APPEND SWARAXT_PLUGIN_FORMATS Standalone)
endif()
if(APPLE)
    list(APPEND SWARAXT_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(SwaraXT
    COMPANY_NAME "MontroneDSP"
    BUNDLE_ID com.MontroneDSP.SwaraXT
    PLUGIN_MANUFACTURER_CODE MnTr
    PLUGIN_CODE SwXT
    FORMATS ${SWARAXT_PLUGIN_FORMATS}
    PRODUCT_NAME "Swara XT"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth
)

juce_generate_juce_header(SwaraXT)

target_sources(SwaraXT PRIVATE ${SWARAXT_ALL_PLUGIN_SOURCES})

target_compile_definitions(SwaraXT
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(SwaraXT
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

swaraxt_enable_warnings(SwaraXT)
swaraxt_enable_warnings_as_errors(SwaraXT)
swaraxt_configure_shruthi_target(SwaraXT)
swaraxt_enable_avrlib_hd_voice(SwaraXT)
add_dependencies(SwaraXT SwaraXtBuiltinTunings)

# Offline Scala/KBM converter. C++17, not linked into the plugin.
add_executable(SwaraXtTuningConverter
    Source/Tools/TuningConverter/TuningConverterMain.cpp
    Source/Tuning/TuningTable.cpp
)
target_include_directories(SwaraXtTuningConverter PRIVATE "${CMAKE_SOURCE_DIR}/Source")
target_compile_features(SwaraXtTuningConverter PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(SwaraXtTuningConverter PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(SwaraXtTuningConverter PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(SWARA_BUILD_SKIN_EDITOR "Build the standalone SWARA XT Skin Editor" ON)
if(SWARA_BUILD_SKIN_EDITOR)
    juce_add_gui_app(SwaraXtSkinEditor
        COMPANY_NAME "MontroneDSP"
        BUNDLE_ID com.MontroneDSP.SwaraXtSkinEditor
        PRODUCT_NAME "SWARA XT Skin Editor"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL FALSE
    )
    juce_generate_juce_header(SwaraXtSkinEditor)
    target_sources(SwaraXtSkinEditor
        PRIVATE
            Source/Tools/SkinEditor/SkinEditorMain.cpp
            Source/Tools/SkinEditor/SkinEditorWindow.cpp
            Source/Tools/SkinEditor/SkinEditorPreview.cpp
            Source/Ui/SwaraXtSkin.cpp
    )
    target_include_directories(SwaraXtSkinEditor PRIVATE "${CMAKE_SOURCE_DIR}/Source")
    target_compile_definitions(SwaraXtSkinEditor
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SwaraXtSkinEditor,JUCE_PRODUCT_NAME>"
            JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
            SWARAXT_VERSION_STRING="${PROJECT_VERSION}"
    )
    target_link_libraries(SwaraXtSkinEditor
        PRIVATE
            SwaraXTFonts
            juce::juce_gui_extra
            juce::juce_graphics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )
    swaraxt_enable_warnings(SwaraXtSkinEditor)
    swaraxt_enable_warnings_as_errors(SwaraXtSkinEditor)
endif()

if(SWARA_BUILD_TESTS)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        include(Qualification)
    endif()
    enable_testing()
    add_subdirectory(Tests)
endif()
