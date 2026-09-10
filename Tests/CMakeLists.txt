# Opt-in test and research targets (SWARA_BUILD_TESTS=ON).

# HD support library unit tests: standalone, no JUCE. Compiled with a strict
# warning-as-error gate so avrlib_hd stays warning-free on the supported compilers.
add_executable(AvrlibHdUnitTests
    avrlib_hd/AvrlibHdUnitTestsMain.cpp
)
target_include_directories(AvrlibHdUnitTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdUnitTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdUnitTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME AvrlibHdUnitTests COMMAND AvrlibHdUnitTests)

# Classic Shruthi resource tables compiled with the classic flag set (they carry
# the audited warning volume, so no -Werror here); consumed by the HD resource
# tests and later by the HD engine.
add_library(AvrlibHdResourceData STATIC
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/resources.cc"
)
swaraxt_configure_shruthi_target(AvrlibHdResourceData)
swaraxt_suppress_legacy_shruthi_warnings(AvrlibHdResourceData)

add_executable(AvrlibHdResourceTests
    avrlib_hd/ResourcesLoaderTestsMain.cpp
)
target_include_directories(AvrlibHdResourceTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdResourceTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdResourceTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_link_libraries(AvrlibHdResourceTests PRIVATE AvrlibHdResourceData)
add_test(NAME AvrlibHdResourceTests COMMAND AvrlibHdResourceTests)

# Classic oscillator compiled with the classic flag set so the parity TU can
# drive shruthi::Oscillator against HdOscillator::RenderNaive.
add_library(AvrlibHdClassicOscObj STATIC
    ${SWARAXT_CLASSIC_OSCILLATOR_SOURCE}
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/random.cc"
)
swaraxt_configure_shruthi_target(AvrlibHdClassicOscObj)
swaraxt_suppress_legacy_shruthi_warnings(AvrlibHdClassicOscObj)
target_link_libraries(AvrlibHdClassicOscObj PRIVATE AvrlibHdResourceData)

# Classic voice/part/patch compiled with the classic flag set so the mod-matrix
# parity TU can drive the real shruthi::Voice::ProcessControlBlock and read its
# 14-bit destinations and source/destination bytes through the debug taps.
add_library(AvrlibHdClassicVoiceObj STATIC
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/part.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/patch.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/parameter.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/note_stack.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/clock.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/system_settings.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/voice_allocator.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi/midi_dispatcher.cc"
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/random.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/sequencer_settings.cc"
    "${SWARAXT_SHRUTHI_ROOT}/avrlib/string.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/resources.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/audio_out.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/storage.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/display.cc"
    "${CMAKE_SOURCE_DIR}/Source/Engine/ShruthiPort/host/editor.cc"
)
swaraxt_configure_shruthi_target(AvrlibHdClassicVoiceObj)
swaraxt_suppress_legacy_shruthi_warnings(AvrlibHdClassicVoiceObj SUPPRESS_VOICE)
target_link_libraries(AvrlibHdClassicVoiceObj PRIVATE AvrlibHdResourceData)
target_compile_definitions(AvrlibHdClassicVoiceObj
    PUBLIC SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)

add_executable(AvrlibHdOscillatorParityTests
    avrlib_hd/OscillatorParityTestsMain.cpp
)
target_include_directories(AvrlibHdOscillatorParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdOscillatorParityTests)
swaraxt_enable_warnings(AvrlibHdOscillatorParityTests)
target_link_libraries(AvrlibHdOscillatorParityTests PRIVATE AvrlibHdClassicOscObj AvrlibHdResourceData)
add_test(NAME AvrlibHdOscillatorParityTests COMMAND AvrlibHdOscillatorParityTests)

add_executable(AvrlibHdOscillatorTests
    avrlib_hd/OscillatorUnitTestsMain.cpp
)
target_include_directories(AvrlibHdOscillatorTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdOscillatorTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdOscillatorTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdOscillatorTests PRIVATE m)
endif()
add_test(NAME AvrlibHdOscillatorTests COMMAND AvrlibHdOscillatorTests)

# Envelope + LFO parity: the Classic Envelope/Lfo are header-only, so the parity
# TU drives them directly against HdEnvelope/HdLfo faithful mode. It carries the
# classic flag set and links the resource tables + the Classic Random state.
add_executable(AvrlibHdEnvLfoParityTests
    avrlib_hd/EnvLfoParityTestsMain.cpp
)
target_include_directories(AvrlibHdEnvLfoParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdEnvLfoParityTests)
swaraxt_enable_warnings(AvrlibHdEnvLfoParityTests)
target_link_libraries(AvrlibHdEnvLfoParityTests PRIVATE AvrlibHdClassicOscObj AvrlibHdResourceData)
add_test(NAME AvrlibHdEnvLfoParityTests COMMAND AvrlibHdEnvLfoParityTests)

# Envelope + LFO HD unit tests (strict -Werror, synthetic tables, no Classic).
add_executable(AvrlibHdEnvLfoTests
    avrlib_hd/EnvLfoUnitTestsMain.cpp
)
target_include_directories(AvrlibHdEnvLfoTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdEnvLfoTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdEnvLfoTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdEnvLfoTests PRIVATE m)
endif()
add_test(NAME AvrlibHdEnvLfoTests COMMAND AvrlibHdEnvLfoTests)

# Sub-osc + transient parity: Classic SubOscillator/TransientGenerator are
# header-only, so the parity TU drives them directly (byte-exact).
add_executable(AvrlibHdSubTransParityTests
    avrlib_hd/SubTransParityTestsMain.cpp
)
target_include_directories(AvrlibHdSubTransParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdSubTransParityTests)
swaraxt_enable_warnings(AvrlibHdSubTransParityTests)
add_test(NAME AvrlibHdSubTransParityTests COMMAND AvrlibHdSubTransParityTests)

# Sub-osc + transient HD unit tests (strict -Werror, no Classic).
add_executable(AvrlibHdSubTransTests
    avrlib_hd/SubTransUnitTestsMain.cpp
)
target_include_directories(AvrlibHdSubTransTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdSubTransTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdSubTransTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdSubTransTests PRIVATE m)
endif()
add_test(NAME AvrlibHdSubTransTests COMMAND AvrlibHdSubTransTests)

# Mod-matrix parity: drives the real shruthi::Voice control block.
add_executable(AvrlibHdModMatrixParityTests
    avrlib_hd/ModMatrixParityTestsMain.cpp
)
target_include_directories(AvrlibHdModMatrixParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdModMatrixParityTests)
swaraxt_enable_warnings(AvrlibHdModMatrixParityTests)
target_compile_definitions(AvrlibHdModMatrixParityTests PRIVATE
    SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)
target_link_libraries(AvrlibHdModMatrixParityTests PRIVATE
    AvrlibHdClassicVoiceObj AvrlibHdClassicOscObj AvrlibHdResourceData)
add_test(NAME AvrlibHdModMatrixParityTests COMMAND AvrlibHdModMatrixParityTests)

# Voice parity: drives partial/full voice block through the classic Part and
# compares the HostAudioRing output with the faithful avrlib_hd::HdVoice.
add_executable(AvrlibHdVoiceParityTests
    avrlib_hd/VoiceParityTestsMain.cpp
)
target_include_directories(AvrlibHdVoiceParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdVoiceParityTests)
swaraxt_enable_warnings(AvrlibHdVoiceParityTests)
target_link_libraries(AvrlibHdVoiceParityTests PRIVATE
    AvrlibHdClassicVoiceObj AvrlibHdClassicOscObj AvrlibHdResourceData)
add_test(NAME AvrlibHdVoiceParityTests COMMAND AvrlibHdVoiceParityTests)

# Mod-matrix HD unit tests (strict -Werror, no Classic).
add_executable(AvrlibHdModMatrixTests
    avrlib_hd/ModMatrixUnitTestsMain.cpp
)
target_include_directories(AvrlibHdModMatrixTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdModMatrixTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdModMatrixTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdModMatrixTests PRIVATE m)
endif()
add_test(NAME AvrlibHdModMatrixTests COMMAND AvrlibHdModMatrixTests)

# Vowel/breath parity: faithfulness of HdVowel::RenderNaive vs the Classic
# Oscillator::RenderVowel (driven through the real shruthi::Oscillator).
add_executable(AvrlibHdVowelParityTests
    avrlib_hd/VowelParityTestsMain.cpp
)
target_include_directories(AvrlibHdVowelParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
swaraxt_configure_shruthi_target(AvrlibHdVowelParityTests)
swaraxt_enable_warnings(AvrlibHdVowelParityTests)
target_link_libraries(AvrlibHdVowelParityTests PRIVATE AvrlibHdClassicOscObj AvrlibHdResourceData)
add_test(NAME AvrlibHdVowelParityTests COMMAND AvrlibHdVowelParityTests)

# Vowel HD unit tests (strict -Werror, synthetic tables, no Classic).
add_executable(AvrlibHdVowelTests
    avrlib_hd/VowelUnitTestsMain.cpp
)
target_include_directories(AvrlibHdVowelTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
)
if(MSVC)
    target_compile_options(AvrlibHdVowelTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdVowelTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdVowelTests PRIVATE m)
endif()
add_test(NAME AvrlibHdVowelTests COMMAND AvrlibHdVowelTests)

# HD VCF parity: kFaithful must be bit-identical to the live engine's
# swaraxt::SwaraXtFilter for the same committed params across a grid of
# Shruthi-derived control values; also pins the updateFilterFromShruthi scaling.
add_executable(AvrlibHdFilterParityTests
    avrlib_hd/FilterParityTestsMain.cpp
)
target_include_directories(AvrlibHdFilterParityTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
    "${CMAKE_SOURCE_DIR}/Source"
)
swaraxt_configure_shruthi_target(AvrlibHdFilterParityTests)
swaraxt_enable_warnings(AvrlibHdFilterParityTests)
add_test(NAME AvrlibHdFilterParityTests COMMAND AvrlibHdFilterParityTests)

# HD VCF unit tests (strict -Werror): mapping, mode budgets, determinism,
# finite-output safety.
add_executable(AvrlibHdFilterTests
    avrlib_hd/FilterUnitTestsMain.cpp
)
target_include_directories(AvrlibHdFilterTests PRIVATE
    "${SWARAXT_INCLUDE_FIRST}"
    "${CMAKE_SOURCE_DIR}/Source"
)
if(MSVC)
    target_compile_options(AvrlibHdFilterTests PRIVATE /W4 /permissive- /WX)
else()
    target_compile_options(AvrlibHdFilterTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT MSVC)
    target_link_libraries(AvrlibHdFilterTests PRIVATE m)
endif()
add_test(NAME AvrlibHdFilterTests COMMAND AvrlibHdFilterTests)

add_executable(SwaraXTTests
    SwaraXtTestsMain.cpp
    ${SWARAXT_SHRUTHI_SOURCES}
    ${SWARAXT_CLASSIC_OSCILLATOR_SOURCE}
    ${SWARAXT_HOST_SOURCES}
)
set_target_properties(SwaraXTTests PROPERTIES OUTPUT_NAME SwaraXTUnitTests)
swaraxt_configure_shruthi_target(SwaraXTTests)
swaraxt_suppress_legacy_shruthi_warnings(SwaraXTTests SUPPRESS_VOICE)
swaraxt_enable_warnings(SwaraXTTests)
add_test(NAME SwaraXTUnitTests COMMAND SwaraXTTests)

function(swaraxt_add_filter_test_target target mode)
    add_executable(${target}
        Filter/FilterTestsMain.cpp
        "${CMAKE_SOURCE_DIR}/Source/Engine/Filter/SwaraXtFilter.cpp"
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

add_executable(SwaraXtFilterTransitionAudit
    Filter/FilterTransitionAuditMain.cpp
    "${CMAKE_SOURCE_DIR}/Source/Engine/Filter/SwaraXtFilter.cpp"
)
target_include_directories(SwaraXtFilterTransitionAudit PRIVATE "${CMAKE_SOURCE_DIR}/Source")
target_compile_definitions(SwaraXtFilterTransitionAudit PRIVATE SWARAXT_FILTER_DIAGNOSTICS=1)
swaraxt_enable_warnings(SwaraXtFilterTransitionAudit)
add_test(NAME SwaraXtFilterTransitionAudit COMMAND SwaraXtFilterTransitionAudit)

add_executable(SwaraXTFilterCpuBench
    Filter/FilterCpuBenchMain.cpp
    "${CMAKE_SOURCE_DIR}/Source/Engine/Filter/SwaraXtFilter.cpp"
)
target_include_directories(SwaraXTFilterCpuBench PRIVATE "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraXTFilterCpuBench)

function(swaraxt_add_plugin_console_test target)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    juce_generate_juce_header(${target})
    target_sources(${target} PRIVATE ${ARGN} ${SWARAXT_ALL_PLUGIN_SOURCES})
    target_compile_definitions(${target}
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
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
    swaraxt_suppress_legacy_shruthi_warnings(${target})
    swaraxt_enable_avrlib_hd_voice(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Full live-engine regression (real processor + MIDI + filter + SRC).
swaraxt_add_plugin_console_test(SwaraXTLiveEngineTests LiveEngine/LiveEngineTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTIdleCpuTests IdleCpu/IdleCpuTestsMain.cpp)
target_compile_definitions(SwaraXTIdleCpuTests PRIVATE SWARAXT_ENABLE_IDLE_CPU_TESTS=1)

swaraxt_add_plugin_console_test(SwaraXTDcDormancyTests Dormancy/DcDormancyTestsMain.cpp)
target_compile_definitions(SwaraXTDcDormancyTests PRIVATE SWARAXT_ENABLE_IDLE_CPU_TESTS=1)

swaraxt_add_plugin_console_test(SwaraXTMultiInstanceTests LiveEngine/MultiInstanceTestsMain.cpp)

function(swaraxt_add_audio_regression_target target mode)
    swaraxt_add_plugin_console_test(${target} AudioRegression/AudioRegressionTestsMain.cpp)
    target_compile_definitions(${target} PRIVATE SWARAXT_AUDIO_TEST_MODE=${mode})
endfunction()

swaraxt_add_audio_regression_target(SwaraXTAudioParityTests 0)
swaraxt_add_audio_regression_target(SwaraXTSignalChainTests 1)
swaraxt_add_audio_regression_target(SwaraXTInitializationTests 2)
swaraxt_add_audio_regression_target(SwaraXTQueueIntegrityTests 3)

function(swaraxt_add_shruthi_timing_target target mode)
    swaraxt_add_plugin_console_test(${target} ShruthiTiming/ShruthiTimingTestsMain.cpp)
    target_compile_definitions(${target} PRIVATE
        SWARAXT_SHRUTHI_TIMING_TEST_MODE=${mode}
        SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1
    )
endfunction()

swaraxt_add_shruthi_timing_target(SwaraXTShruthiTimingTests 0)
swaraxt_add_shruthi_timing_target(SwaraXTRawOscillatorParityTests 1)
swaraxt_add_shruthi_timing_target(SwaraXTControlRateTests 2)
swaraxt_add_shruthi_timing_target(SwaraXTModulationNeutralityTests 3)
swaraxt_add_shruthi_timing_target(SwaraXTStreamingSrcTests 4)
swaraxt_add_shruthi_timing_target(SwaraXTChunkContinuityTests 5)
swaraxt_add_shruthi_timing_target(SwaraXTEnvelopeRegressionTests 6)
swaraxt_add_shruthi_timing_target(SwaraXTVcaHoldTests 7)

swaraxt_add_plugin_console_test(SwaraXTCutoffRefineTests Filter/CutoffRefineTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTGuiSyncTests GuiSync/GuiSyncTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTShruthiFactoryPresetTests Presets/ShruthiFactoryPresetTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTPresetStateRestoreTests Presets/PresetStateRestoreTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTSequencerTests Sequencer/SequencerTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTCorrectnessTests Correctness/CorrectnessTestsMain.cpp)
target_compile_definitions(SwaraXTCorrectnessTests PRIVATE SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)
# Direct Classic Oscillator PWM/oracle checks; production plugin does not compile this TU.
target_sources(SwaraXTCorrectnessTests PRIVATE ${SWARAXT_CLASSIC_OSCILLATOR_SOURCE})

swaraxt_add_plugin_console_test(SwaraXTSmoothingAuditTests Smoothing/SmoothingAuditTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTArchitectureControlTests Architecture/FilterControlTests.cpp)
target_compile_definitions(SwaraXTArchitectureControlTests PRIVATE SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)

add_executable(SwaraXTFilterMultirateTests Architecture/FilterMultirateTests.cpp)
target_include_directories(SwaraXTFilterMultirateTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
target_compile_definitions(SwaraXTFilterMultirateTests PRIVATE SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)
swaraxt_enable_warnings(SwaraXTFilterMultirateTests)
add_test(NAME SwaraXTFilterMultirateTests COMMAND SwaraXTFilterMultirateTests)

add_executable(SwaraXTIR3109BoardTests Architecture/Ir3109BoardTests.cpp)
target_include_directories(SwaraXTIR3109BoardTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraXTIR3109BoardTests)
add_test(NAME SwaraXTIR3109BoardTests COMMAND SwaraXTIR3109BoardTests)

add_executable(SwaraXTHostSrcTests Architecture/HostSrcTests.cpp)
target_include_directories(SwaraXTHostSrcTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraXTHostSrcTests)
add_test(NAME SwaraXTHostSrcTests COMMAND SwaraXTHostSrcTests)

swaraxt_add_plugin_console_test(SwaraXTConditioningTests Conditioning/ConditioningTestsMain.cpp)
target_compile_definitions(SwaraXTConditioningTests PRIVATE SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)

# SRC quality study harnesses (research only; not registered with add_test).
function(swaraxt_add_src_quality_target target taps)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    juce_generate_juce_header(${target})
    target_sources(${target}
        PRIVATE
            "${CMAKE_SOURCE_DIR}/Tools/SrcQuality/SrcEngineHarness.cpp"
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
    swaraxt_suppress_legacy_shruthi_warnings(${target})
    swaraxt_enable_avrlib_hd_voice(${target})
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

swaraxt_add_src_quality_target(SwaraXTSrcStudyHermite 0)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir64 64)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir96 96)

add_subdirectory(DspBoard)

# Cross-build classic+/main parity dump. Invoked manually, not registered with CTest.
juce_add_console_app(SwaraXTParityDump PRODUCT_NAME "SwaraXTParityDump")
juce_generate_juce_header(SwaraXTParityDump)
target_sources(SwaraXTParityDump PRIVATE Parity/ParityDumpMain.cpp ${SWARAXT_ALL_PLUGIN_SOURCES})
target_compile_definitions(SwaraXTParityDump
    PRIVATE
        SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SwaraXTParityDump
    PRIVATE
        SwaraXTFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
swaraxt_enable_warnings(SwaraXTParityDump)
swaraxt_configure_shruthi_target(SwaraXTParityDump)
swaraxt_suppress_legacy_shruthi_warnings(SwaraXTParityDump)
swaraxt_enable_avrlib_hd_voice(SwaraXTParityDump)
