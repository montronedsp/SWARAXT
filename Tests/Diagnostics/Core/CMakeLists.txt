cmake_minimum_required(VERSION 3.22)
project(SwaraXTCoreQualification LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "Core qualification supports Linux and Windows")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(SWARAXT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${SWARAXT_SOURCE_ROOT}/cmake/Qualification.cmake")
set(SWARAXT_SHRUTHI_ROOT "${SWARAXT_SOURCE_ROOT}/third_party/shruthi-1")
set(SWARAXT_SHRUTHI_ADAPTED_ROOT "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/adapted")
set(SWARAXT_INCLUDE_FIRST "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/include_first")
include("${SWARAXT_SOURCE_ROOT}/cmake/ShruthiRomResources.cmake")
enable_testing()

function(core_target target)
    target_include_directories(${target} PRIVATE
        "${SWARAXT_SOURCE_ROOT}/Source" "${SWARAXT_INCLUDE_FIRST}"
        "${SWARAXT_SHRUTHI_ADAPTED_ROOT}" "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/shruthi"
        "${SWARAXT_SHRUTHI_ROOT}" "${SWARAXT_SHRUTHI_ROOT}/shruthi" "${SWARAXT_SHRUTHI_ROOT}/midi")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- "/FI${SWARAXT_INCLUDE_FIRST}/swaraxt/shruthi_host.h")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(SWARAXT_DIAGNOSTICS_MODULE)
        diag_apply(${target})
    endif()
endfunction()
function(core_test target source)
    add_executable(${target} "${SWARAXT_SOURCE_ROOT}/Tests/${source}")
    core_target(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 300 LABELS "quick;core")
endfunction()

add_library(SwaraXTCoreRom STATIC "${SWARAXT_SHRUTHI_RESOURCE_SOURCE}")
core_target(SwaraXTCoreRom)
add_library(SwaraXTCoreVoice STATIC
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
    "${SWARAXT_SHRUTHI_ADAPTED_ROOT}/avrlib/string.cc"
    "${SWARAXT_SHRUTHI_ROOT}/shruthi/sequencer_settings.cc"
    "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/host/audio_out.cc"
    "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/host/storage.cc"
    "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/host/display.cc"
    "${SWARAXT_SOURCE_ROOT}/Source/Engine/ShruthiPort/host/editor.cc")
core_target(SwaraXTCoreVoice)
target_compile_definitions(SwaraXTCoreVoice PUBLIC SWARAXT_USE_AVRLIB_HD_VOICE=1)
target_link_libraries(SwaraXTCoreVoice PRIVATE SwaraXTCoreRom)
core_test(SwaraXTCoreVoiceTests SwaraXtTestsMain.cpp)
target_link_libraries(SwaraXTCoreVoiceTests PRIVATE SwaraXTCoreVoice)
core_test(SwaraXTArpeggiatorBoundaryTests Diagnostics/ArpeggiatorBoundaryTestsMain.cpp)
target_link_libraries(SwaraXTArpeggiatorBoundaryTests PRIVATE SwaraXTCoreVoice)
find_package(Threads REQUIRED)
target_link_libraries(SwaraXTCoreVoiceTests PRIVATE Threads::Threads)

core_test(AvrlibHdUpperRegisterOscillatorTests avrlib_hd/UpperRegisterOscillatorTestsMain.cpp)
target_link_libraries(AvrlibHdUpperRegisterOscillatorTests PRIVATE SwaraXTCoreRom)
core_test(AvrlibHdModMatrixTests avrlib_hd/ModMatrixUnitTestsMain.cpp)
core_test(AvrlibHdOscillatorTests avrlib_hd/OscillatorUnitTestsMain.cpp)
core_test(SwaraXTHostSrcExactParityTests Parity/HostSrcExactParityTestsMain.cpp)
target_include_directories(SwaraXTHostSrcExactParityTests PRIVATE "${SWARAXT_SOURCE_ROOT}/Tests")
core_test(SwaraXTTuningTableTests Tuning/TuningTableTestsMain.cpp)
target_sources(SwaraXTTuningTableTests PRIVATE "${SWARAXT_SOURCE_ROOT}/Source/Tuning/TuningTable.cpp")

if(SWARAXT_DIAGNOSTICS_MODULE AND NOT DIAG_FUZZER STREQUAL "OFF")
    add_executable(SwaraXTTuningFuzz "${SWARAXT_SOURCE_ROOT}/Tests/Fuzz/TuningFuzz.cpp"
        "${SWARAXT_SOURCE_ROOT}/Source/Tuning/TuningTable.cpp")
    core_target(SwaraXTTuningFuzz)
    if(DIAG_FUZZER STREQUAL "libfuzzer")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "LibFuzzer requires Clang")
        endif()
        target_compile_options(SwaraXTTuningFuzz PRIVATE -fsanitize=fuzzer-no-link)
    elseif(NOT DIAG_FUZZER STREQUAL "afl")
        message(FATAL_ERROR "Unsupported qualification fuzzer")
    endif()
    diag_fuzz_target(SwaraXTTuningFuzz)
endif()

if(SWARAXT_DIAGNOSTICS_MODULE AND DIAG_SANITIZER STREQUAL "rtsan")
    add_executable(SwaraXTCoreRealtimeTests "${SWARAXT_SOURCE_ROOT}/Tests/Diagnostics/CoreRealtimeTestsMain.cpp")
    core_target(SwaraXTCoreRealtimeTests)
    target_link_libraries(SwaraXTCoreRealtimeTests PRIVATE SwaraXTCoreVoice)
    foreach(scenario notes arp modulation)
        add_test(NAME SwaraXTCoreRealtime-${scenario} COMMAND SwaraXTCoreRealtimeTests ${scenario})
        set_tests_properties(SwaraXTCoreRealtime-${scenario} PROPERTIES LABELS "sanitizer;core"
            TIMEOUT 120 FAIL_REGULAR_EXPRESSION "RealtimeSanitizer:;SUMMARY: RealtimeSanitizer")
    endforeach()
endif()
