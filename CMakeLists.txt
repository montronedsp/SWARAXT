cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
        "Minimum macOS deployment target")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING
        "macOS architectures")
endif()

project(SwaraXT VERSION 1.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerWarnings)

option(SWARA_BUILD_TESTS "Build SWARA XT test executables" OFF)

set(SWARAXT_SHRUTHI_ROOT "${CMAKE_SOURCE_DIR}/third_party/shruthi-1")
set(SWARAXT_SHRUTHI_ADAPTED_ROOT "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/adapted")
set(SWARAXT_INCLUDE_FIRST "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/include_first")

set(SWARAXT_SHRUTHI_SOURCES
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/oscillator.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/resources.cc"
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
    "${SWARAXT_SHRUTHI_ROOT}/avrlib/string.cc"
)

set(SWARAXT_HOST_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/audio_out.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/display.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/editor.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/storage.cc"
)

set(SWARAXT_ENGINE_SOURCES
    "${CMAKE_SOURCE_DIR}/Source/Engine/PatchBridge.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/SwaraXtEngine.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/SampleRate/HostResampler.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/Filter/SwaraXtFilter.cpp"
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
    "${CMAKE_SOURCE_DIR}/Source/Ui/SwaraXtSkin.cpp"
)

juce_add_binary_data(SwaraXTFonts
    SOURCES
        resources/Fonts/din1451alt.ttf
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/braid.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/company_wordmark.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/jam_ornament.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/midnight_backdrop.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/midnight_product_lockup.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/mod_matrix_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/MidnightGold/visualizer_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_backdrop.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_braid.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_company_wordmark.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_jam_ornament.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_mod_matrix_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_product_lockup.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/NeonCobalt/neon_visualizer_frame.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/vco1_header.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Shared/vco2_header.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Skin/Decorations/pcb_trace_separator.svg"
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
    if(MSVC)
        target_compile_options(${target} PRIVATE /wd4244 /wd4267 /wd4305 /wd4309 /wd4716)
    else()
        target_compile_options(${target} PRIVATE -Wno-sign-compare -Wno-unused-parameter)
        if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-c++11-narrowing)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # Shruthi AVR tables use signed literals in unsigned brace initializers.
            target_compile_options(${target} PRIVATE -Wno-narrowing)
        endif()
    endif()
endfunction()

set(SWARAXT_PLUGIN_FORMATS VST3 Standalone)
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
    NEEDS_MIDI_OUTPUT FALSE
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
swaraxt_configure_shruthi_target(SwaraXT)

if(SWARA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
