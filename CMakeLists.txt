cmake_minimum_required(VERSION 3.22)

project(SwaraXT VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerWarnings)
enable_testing()

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
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/note_stack.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/clock.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/sequencer_settings.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/system_settings.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice_allocator.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/midi_dispatcher.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/random.cc"
    "${SWARAXT_SHRUTHI_ROOT}/avrlib/string.cc"
)

set(SWARAXT_HOST_SOURCES
    Source/Engine/ShruthiPort/host/audio_out.cc
    Source/Engine/ShruthiPort/host/display.cc
    Source/Engine/ShruthiPort/host/editor.cc
    Source/Engine/ShruthiPort/host/storage.cc
)

set(SWARAXT_ENGINE_SOURCES
    Source/Engine/PatchBridge.cpp
    Source/Engine/SwaraXtEngine.cpp
    Source/Engine/SampleRate/HostResampler.cpp
    Source/Engine/Filter/SwaraXtFilter.cpp
)

set(SWARAXT_PLUGIN_SOURCES
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/Plugin/SwaraXtParameterLayout.cpp
)

set(SWARAXT_UI_SOURCES
    Source/Ui/SwaraXtLookAndFeel.cpp
    Source/Ui/SwaraXtPanels.cpp
    Source/Ui/SwaraXtSkin.cpp
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
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # Shruthi AVR tables use signed literals in unsigned brace initializers.
            target_compile_options(${target} PRIVATE -Wno-narrowing)
        endif()
    endif()
endfunction()

juce_add_plugin(SwaraXT
    COMPANY_NAME "MontroneDSP"
    BUNDLE_ID com.MontroneDSP.SwaraXT
    PLUGIN_MANUFACTURER_CODE MnTr
    PLUGIN_CODE SwXT
    FORMATS VST3 Standalone
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
        JUCE_DISPLAY_SPLASH_SCREEN=0
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

add_executable(SwaraXTTests
    Tests/SwaraXtTestsMain.cpp
    ${SWARAXT_SHRUTHI_SOURCES}
    ${SWARAXT_HOST_SOURCES}
)
set_target_properties(SwaraXTTests PROPERTIES OUTPUT_NAME SwaraXTUnitTests)
swaraxt_configure_shruthi_target(SwaraXTTests)
swaraxt_enable_warnings(SwaraXTTests)
add_test(NAME SwaraXTUnitTests COMMAND SwaraXTTests)

function(swaraxt_add_filter_test_target target mode)
    add_executable(${target}
        Tests/Filter/FilterTestsMain.cpp
        Source/Engine/Filter/SwaraXtFilter.cpp
    )
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/Source")
    target_compile_definitions(${target} PRIVATE
        SWARAXT_FILTER_TEST_MODE=${mode}
        SWARAXT_FILTER_DIAGNOSTICS=1
    )
    swaraxt_enable_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

swaraxt_add_filter_test_target(SwaraXtFilterTests 0)
swaraxt_add_filter_test_target(SwaraXtFilterCoreTests 1)
swaraxt_add_filter_test_target(SwaraXtFilterDcTests 2)
swaraxt_add_filter_test_target(SwaraXtFilterResponseTests 3)
swaraxt_add_filter_test_target(SwaraXtFilterResonanceTests 4)
swaraxt_add_filter_test_target(SwaraXtFilterModulationTests 5)
swaraxt_add_filter_test_target(SwaraXtFilterOversamplingTests 6)
swaraxt_add_filter_test_target(SwaraXtFilterIntegrationTests 7)

# Full live-engine regression (real processor + MIDI + filter + SRC).
juce_add_console_app(SwaraXTLiveEngineTests PRODUCT_NAME "SwaraXTLiveEngineTests")
juce_generate_juce_header(SwaraXTLiveEngineTests)
target_sources(SwaraXTLiveEngineTests
    PRIVATE
        Tests/LiveEngine/LiveEngineTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTLiveEngineTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTLiveEngineTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTLiveEngineTests)
swaraxt_configure_shruthi_target(SwaraXTLiveEngineTests)
add_test(NAME SwaraXTLiveEngineTests COMMAND SwaraXTLiveEngineTests)

juce_add_console_app(SwaraXTIdleCpuTests PRODUCT_NAME "SwaraXTIdleCpuTests")
juce_generate_juce_header(SwaraXTIdleCpuTests)
target_sources(SwaraXTIdleCpuTests
    PRIVATE
        Tests/IdleCpu/IdleCpuTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTIdleCpuTests
    PRIVATE
        SWARAXT_ENABLE_IDLE_CPU_TESTS=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTIdleCpuTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTIdleCpuTests)
swaraxt_configure_shruthi_target(SwaraXTIdleCpuTests)
add_test(NAME SwaraXTIdleCpuTests COMMAND SwaraXTIdleCpuTests)

juce_add_console_app(SwaraXTMultiInstanceTests PRODUCT_NAME "SwaraXTMultiInstanceTests")
juce_generate_juce_header(SwaraXTMultiInstanceTests)
target_sources(SwaraXTMultiInstanceTests
    PRIVATE
        Tests/LiveEngine/MultiInstanceTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTMultiInstanceTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTMultiInstanceTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTMultiInstanceTests)
swaraxt_configure_shruthi_target(SwaraXTMultiInstanceTests)
add_test(NAME SwaraXTMultiInstanceTests COMMAND SwaraXTMultiInstanceTests)

function(swaraxt_add_audio_regression_target target mode)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    juce_generate_juce_header(${target})
    target_sources(${target}
        PRIVATE
            Tests/AudioRegression/AudioRegressionTestsMain.cpp
            ${SWARAXT_ALL_PLUGIN_SOURCES}
    )
    target_compile_definitions(${target}
        PRIVATE
            SWARAXT_AUDIO_TEST_MODE=${mode}
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
    )
    target_link_libraries(${target}
        PRIVATE
            SwaraXTFonts
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    swaraxt_enable_warnings(${target})
    swaraxt_configure_shruthi_target(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

swaraxt_add_audio_regression_target(SwaraXTAudioParityTests 0)
swaraxt_add_audio_regression_target(SwaraXTSignalChainTests 1)
swaraxt_add_audio_regression_target(SwaraXTInitializationTests 2)
swaraxt_add_audio_regression_target(SwaraXTQueueIntegrityTests 3)

function(swaraxt_add_shruthi_timing_target target mode)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    juce_generate_juce_header(${target})
    target_sources(${target}
        PRIVATE
            Tests/ShruthiTiming/ShruthiTimingTestsMain.cpp
            ${SWARAXT_ALL_PLUGIN_SOURCES}
    )
    target_compile_definitions(${target}
        PRIVATE
            SWARAXT_SHRUTHI_TIMING_TEST_MODE=${mode}
            SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
    )
    target_link_libraries(${target}
        PRIVATE
            SwaraXTFonts
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    swaraxt_enable_warnings(${target})
    swaraxt_configure_shruthi_target(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

swaraxt_add_shruthi_timing_target(SwaraXTShruthiTimingTests 0)
swaraxt_add_shruthi_timing_target(SwaraXTRawOscillatorParityTests 1)
swaraxt_add_shruthi_timing_target(SwaraXTControlRateTests 2)
swaraxt_add_shruthi_timing_target(SwaraXTModulationNeutralityTests 3)
swaraxt_add_shruthi_timing_target(SwaraXTStreamingSrcTests 4)
swaraxt_add_shruthi_timing_target(SwaraXTChunkContinuityTests 5)
swaraxt_add_shruthi_timing_target(SwaraXTEnvelopeRegressionTests 6)

juce_add_console_app(SwaraXTCutoffRefineTests PRODUCT_NAME "SwaraXTCutoffRefineTests")
juce_generate_juce_header(SwaraXTCutoffRefineTests)
target_sources(SwaraXTCutoffRefineTests
    PRIVATE
        Tests/Filter/CutoffRefineTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTCutoffRefineTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTCutoffRefineTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTCutoffRefineTests)
swaraxt_configure_shruthi_target(SwaraXTCutoffRefineTests)
add_test(NAME SwaraXTCutoffRefineTests COMMAND SwaraXTCutoffRefineTests)

juce_add_console_app(SwaraXTGuiSyncTests PRODUCT_NAME "SwaraXTGuiSyncTests")
juce_generate_juce_header(SwaraXTGuiSyncTests)
target_sources(SwaraXTGuiSyncTests
    PRIVATE
        Tests/GuiSync/GuiSyncTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTGuiSyncTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTGuiSyncTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTGuiSyncTests)
swaraxt_configure_shruthi_target(SwaraXTGuiSyncTests)
add_test(NAME SwaraXTGuiSyncTests COMMAND SwaraXTGuiSyncTests)

juce_add_console_app(SwaraXTCorrectnessTests PRODUCT_NAME "SwaraXTCorrectnessTests")
juce_generate_juce_header(SwaraXTCorrectnessTests)
target_sources(SwaraXTCorrectnessTests
    PRIVATE
        Tests/Correctness/CorrectnessTestsMain.cpp
        ${SWARAXT_ALL_PLUGIN_SOURCES}
)
target_compile_definitions(SwaraXTCorrectnessTests
    PRIVATE
        SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(SwaraXTCorrectnessTests
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
swaraxt_enable_warnings(SwaraXTCorrectnessTests)
swaraxt_configure_shruthi_target(SwaraXTCorrectnessTests)
add_test(NAME SwaraXTCorrectnessTests COMMAND SwaraXTCorrectnessTests)

# --------------------------------------------------------------------------
# SRC quality study (linux-src-quality branch).
#
# One harness source built once per candidate converter so the Hermite reader
# and the polyphase FIR can be measured through the real plugin. These targets
# are research tools: they are not part of the release test suite and are not
# registered with add_test().
function(swaraxt_add_src_quality_target target taps)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    juce_generate_juce_header(${target})
    target_sources(${target}
        PRIVATE
            Tools/SrcQuality/SrcEngineHarness.cpp
            ${SWARAXT_ALL_PLUGIN_SOURCES}
    )
    target_compile_definitions(${target}
        PRIVATE
            SWARAXT_SRC_FIR_TAPS=${taps}
            SWARAXT_ENABLE_IDLE_CPU_TESTS=1
            SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
    )
    target_link_libraries(${target}
        PRIVATE
            SwaraXTFonts
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    swaraxt_enable_warnings(${target})
    swaraxt_configure_shruthi_target(${target})
endfunction()

swaraxt_add_src_quality_target(SwaraXTSrcStudyHermite 0)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir64 64)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir96 96)
