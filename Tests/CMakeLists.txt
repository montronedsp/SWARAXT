# Opt-in test and research targets (SWARA_BUILD_TESTS=ON).

add_executable(SwaraXTTests
    SwaraXtTestsMain.cpp
    ${SWARAXT_SHRUTHI_SOURCES}
    ${SWARAXT_HOST_SOURCES}
)
set_target_properties(SwaraXTTests PROPERTIES OUTPUT_NAME SwaraXTUnitTests)
swaraxt_configure_shruthi_target(SwaraXTTests)
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

swaraxt_add_plugin_console_test(SwaraXTCutoffRefineTests Filter/CutoffRefineTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTGuiSyncTests GuiSync/GuiSyncTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTShruthiFactoryPresetTests Presets/ShruthiFactoryPresetTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTPresetStateRestoreTests Presets/PresetStateRestoreTestsMain.cpp)

swaraxt_add_plugin_console_test(SwaraXTCorrectnessTests Correctness/CorrectnessTestsMain.cpp)
target_compile_definitions(SwaraXTCorrectnessTests PRIVATE SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS=1)

swaraxt_add_plugin_console_test(SwaraXTSmoothingAuditTests Smoothing/SmoothingAuditTestsMain.cpp)

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
endfunction()

swaraxt_add_src_quality_target(SwaraXTSrcStudyHermite 0)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir64 64)
swaraxt_add_src_quality_target(SwaraXTSrcStudyFir96 96)
