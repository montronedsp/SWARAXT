# Included only for Linux/Windows test builds.
function(swaraxt_qualify_directory directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE" AND NOT target MATCHES "^(SwaraXTFilterCpuBench|SwaraXTParityDump|SwaraXTSrcStudy|SwaraXTTuningFuzz|SwaraXTStateMidiFuzz)")
            set_property(GLOBAL APPEND PROPERTY SWARAXT_QUALIFICATION_TARGETS ${target})
        endif()
        if(type STREQUAL "EXECUTABLE" OR type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "OBJECT_LIBRARY")
            if(SWARAXT_DIAGNOSTICS_MODULE)
                diag_apply(${target})
                if(DIAG_FUZZER STREQUAL "libfuzzer")
                    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
                endif()
            endif()
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        swaraxt_qualify_directory("${child}")
    endforeach()
endfunction()

if(SWARAXT_DIAGNOSTICS_MODULE AND DIAG_FUZZER STREQUAL "libfuzzer")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "State/MIDI LibFuzzer requires Clang")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/StateMidiFuzz.cmake")
endif()

swaraxt_add_plugin_console_test(SwaraXTArpeggiatorBoundaryTests Diagnostics/ArpeggiatorBoundaryTestsMain.cpp)
swaraxt_add_plugin_console_test(SwaraXTMidiLifecycleTests Diagnostics/MidiLifecycleTestsMain.cpp)
add_test(NAME SwaraXTMidiOutputLifecycle COMMAND SwaraXTMidiLifecycleTests output)
add_test(NAME SwaraXTMidiSaturation COMMAND SwaraXTMidiLifecycleTests saturation)
add_test(NAME SwaraXTStandaloneMidiLifecycle COMMAND SwaraXTMidiLifecycleTests standalone)

if(SWARAXT_DIAGNOSTICS_MODULE AND DIAG_SANITIZER STREQUAL "rtsan")
    swaraxt_add_plugin_console_test(SwaraXTRealtimeTests Diagnostics/RealtimeTestsMain.cpp)
    # Each scenario runs in a separate process to avoid first-report masking.
    set_tests_properties(SwaraXTRealtimeTests PROPERTIES
        LABELS sanitizer TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "RealtimeSanitizer:;SUMMARY: RealtimeSanitizer")
    foreach(scenario notes sequencer restore)
        add_test(NAME SwaraXTRealtime-${scenario} COMMAND SwaraXTRealtimeTests ${scenario})
        set_tests_properties(SwaraXTRealtime-${scenario} PROPERTIES LABELS sanitizer TIMEOUT 120
            FAIL_REGULAR_EXPRESSION "RealtimeSanitizer:;SUMMARY: RealtimeSanitizer")
    endforeach()
endif()

# Build JUCE once per compatible module configuration within this build tree.
# Application/engine TUs retain all original target-specific definitions.
# Libraries are never reused across build trees, compilers or instrumentation.
if(SWARAXT_SHARE_TEST_JUCE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_property(console_targets GLOBAL PROPERTY SWARAXT_CONSOLE_TEST_TARGETS)
    foreach(target IN LISTS console_targets)
        get_target_property(definitions ${target} COMPILE_DEFINITIONS)
        set(modal 0)
        if("JUCE_MODAL_LOOPS_PERMITTED=1" IN_LIST definitions)
            set(modal 1)
        endif()
        set(support SwaraXTTestJuce${modal})
        if(NOT TARGET ${support})
            add_library(${support} STATIC)
            target_compile_definitions(${support} PRIVATE
                JUCE_STANDALONE_APPLICATION=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_MODAL_LOOPS_PERMITTED=${modal})
            target_link_libraries(${support} PRIVATE juce::juce_audio_utils juce::juce_dsp
                juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
        endif()
        get_target_property(links ${target} LINK_LIBRARIES)
        list(REMOVE_ITEM links juce::juce_audio_utils juce::juce_dsp)
        set_property(TARGET ${target} PROPERTY LINK_LIBRARIES "${links}")
        target_link_libraries(${target} PRIVATE ${support})
        # Export evaluated module usage requirements without INTERFACE_SOURCES.
        target_include_directories(${target} PRIVATE "$<TARGET_PROPERTY:${support},INCLUDE_DIRECTORIES>")
        target_compile_definitions(${target} PRIVATE "$<TARGET_PROPERTY:${support},COMPILE_DEFINITIONS>")
    endforeach()
endif()

# Share production objects only when the complete direct target environment
# matches. Keep every test-specific definition in the signature, including
# diagnostic taps and test modes. Generated JUCE headers have the same module
# includes; their ProjectInfo names are not used by production sources.
if(SWARAXT_SHARE_TEST_PRODUCTION AND SWARAXT_SHARE_TEST_JUCE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_property(console_targets GLOBAL PROPERTY SWARAXT_CONSOLE_TEST_TARGETS)
    set(groups "")
    foreach(target IN LISTS console_targets)
        get_target_property(definitions ${target} COMPILE_DEFINITIONS)
        get_target_property(options ${target} COMPILE_OPTIONS)
        get_target_property(includes ${target} INCLUDE_DIRECTORIES)
        get_target_property(links ${target} LINK_LIBRARIES)
        # Only this generated include directory depends on executable name.
        list(REMOVE_ITEM includes "${CMAKE_CURRENT_BINARY_DIR}/${target}_artefacts/JuceLibraryCode")
        list(REMOVE_ITEM includes "$<TARGET_PROPERTY:${target},JUCE_GENERATED_SOURCES_DIRECTORY>")
        string(SHA256 signature "${definitions}|${options}|${includes}|${links}")
        list(APPEND groups ${signature})
        list(APPEND members_${signature} ${target})
    endforeach()
    list(REMOVE_DUPLICATES groups)
    foreach(signature IN LISTS groups)
        list(LENGTH members_${signature} count)
        if(count LESS 2)
            continue()
        endif()
        list(GET members_${signature} 0 first)
        string(SUBSTRING "${signature}" 0 12 short_signature)
        set(support SwaraXTTestProduction_${short_signature})
        get_target_property(definitions ${first} COMPILE_DEFINITIONS)
        get_target_property(options ${first} COMPILE_OPTIONS)
        get_target_property(includes ${first} INCLUDE_DIRECTORIES)
        get_target_property(links ${first} LINK_LIBRARIES)
        add_library(${support} OBJECT ${SWARAXT_ALL_PLUGIN_SOURCES}
            "${CMAKE_CURRENT_BINARY_DIR}/${first}_artefacts/JuceLibraryCode/JuceHeader.h")
        target_compile_definitions(${support} PRIVATE ${definitions})
        target_compile_options(${support} PRIVATE ${options})
        target_include_directories(${support} PRIVATE ${includes})
        target_link_libraries(${support} PRIVATE ${links})
        add_dependencies(${support} SwaraXtBuiltinTunings)
        foreach(target IN LISTS members_${signature})
            get_target_property(sources ${target} SOURCES)
            list(REMOVE_ITEM sources ${SWARAXT_ALL_PLUGIN_SOURCES})
            set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
            target_link_libraries(${target} PRIVATE ${support})
        endforeach()
    endforeach()
endif()

set_tests_properties(SwaraXTSonicQualityExtended PROPERTIES DISABLED FALSE)
# Both profiles export evidence; separate directories avoid overwriting a
# completed profile when they run sequentially or concurrently.
foreach(profile quick extended)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sonic-${profile}")
endforeach()
set_tests_properties(SwaraXTSonicQualityTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sonic-quick")
set_tests_properties(SwaraXTSonicQualityExtended PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sonic-extended")
set_tests_properties(SwaraXTSonicQualityTests SwaraXTSonicQualityExtended PROPERTIES
    FAIL_REGULAR_EXPRESSION "NEEDS_REVIEW [1-9][0-9]*")
set_tests_properties(SwaraXTDcDormancyTests SwaraXTIdleCpuTests SwaraXTSmoothingAuditTests
    SwaraXTMidiInputTuningTests SwaraXTCutoffRefineTests
    SwaraXTFilterMultirateTests SwaraXTIR3109BoardTests PROPERTIES LABELS extended TIMEOUT 1800)
# The aggregate filter target duplicates the seven focused modes; retain it as
# an extended consistency run instead of paying for both in the quick gate.
set_tests_properties(SwaraXtFilterTests PROPERTIES LABELS extended TIMEOUT 1800)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME SwaraXTFactoryGeneratorTests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Presets/FactoryGeneratorTests.py")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME SwaraXTQualificationArgumentTests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Diagnostics/VerifyQualificationTests.py")
    set_tests_properties(SwaraXTQualificationArgumentTests PROPERTIES LABELS quick TIMEOUT 30)
endif()



if(SWARAXT_DIAGNOSTICS_MODULE AND NOT DIAG_FUZZER STREQUAL "OFF")
    add_executable(SwaraXTTuningFuzz Fuzz/TuningFuzz.cpp "${CMAKE_SOURCE_DIR}/Source/Tuning/TuningTable.cpp")
    target_include_directories(SwaraXTTuningFuzz PRIVATE "${CMAKE_SOURCE_DIR}/Source")
    if(DIAG_FUZZER STREQUAL "libfuzzer")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "LibFuzzer requires Clang")
        endif()
        target_compile_options(SwaraXTTuningFuzz PRIVATE -fsanitize=fuzzer-no-link)
    elseif(NOT DIAG_FUZZER STREQUAL "afl")
        message(FATAL_ERROR "Unknown qualification fuzzer")
    endif()
    diag_fuzz_target(SwaraXTTuningFuzz)
endif()

if(SWARAXT_DIAGNOSTICS_MODULE AND DIAG_SANITIZER STREQUAL "rtsan")
    set_tests_properties(SwaraXTMidiLifecycleTests SwaraXTMidiOutputLifecycle
        SwaraXTMidiSaturation SwaraXTStandaloneMidiLifecycle PROPERTIES
        LABELS sanitizer TIMEOUT 120 FAIL_REGULAR_EXPRESSION "RealtimeSanitizer:;SUMMARY: RealtimeSanitizer")
    # This caller deliberately violates the supported wrapper capacity contract.
    # Retain it as a negative control that requires the original allocation report.
    add_test(NAME SwaraXTMidiUnreservedOutput COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:SwaraXTMidiLifecycleTests>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/Support/ExpectMidiCapacityDiagnostic.cmake")
    set_tests_properties(SwaraXTMidiUnreservedOutput PROPERTIES
        LABELS "sanitizer;negative-control" TIMEOUT 120)
endif()
swaraxt_qualify_directory("${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    set_tests_properties(SwaraXtShruthiSvf2pSanitizerTests SwaraBoardSanitizerTests
        SwaraBoardIntegrationSanitizerTests
        PROPERTIES LABELS sanitizer TIMEOUT 600)
endif()
swaraxt_label_tests()
get_property(qualification_targets GLOBAL PROPERTY SWARAXT_QUALIFICATION_TARGETS)
add_custom_target(SwaraXTQualificationTests DEPENDS ${qualification_targets})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(SwaraXTTuningTableTests PRIVATE -Wno-error=type-limits)
endif()
