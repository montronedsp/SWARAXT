if(MSVC)
    # ASan replaces /RTC1 instrumentation in this new, isolated test directory.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

add_library(SwaraBoardAlgorithm STATIC
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardResources.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardFilter.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardEffects.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardProcessor.cpp")
target_include_directories(SwaraBoardAlgorithm PUBLIC "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraBoardAlgorithm)

# Only the test executable links the original firmware oracle. Its signed
# left shift is expressed as bounded multiplication in a build-local overlay;
# the original submodule and the production implementation remain unchanged.
set(reference_fx_source "${SWARAXT_SHRUTHI_ROOT}/dsp/fx_engine.cc")
file(READ "${reference_fx_source}" reference_fx)
set(reference_shift "pole_[0] - pole_[1], resonance) << 2;")
string(FIND "${reference_fx}" "${reference_shift}" reference_shift_position)
if(reference_shift_position LESS 0)
    message(FATAL_ERROR "Supported firmware oracle shift anchor changed")
endif()
# S16U8MulShift8 returns int16_t: multiplication by four fits a 32-bit int.
# The existing int16_t assignment retains the original target wrap semantics.
string(REPLACE "${reference_shift}" "pole_[0] - pole_[1], resonance) * 4;"
    reference_fx "${reference_fx}")
# Perform the original cheap modulo before forming a pointer. Constructing the
# temporary pointer beyond the array is undefined even when wrapped before use.
set(reference_delay [=[  int8_t* read_ptr = &fx_state_.delay_line[
      (fx_state_.delay_ptr - delay_duration + delay_line_size)];
  while (read_ptr >= end) {
    read_ptr -= delay_line_size;  // Cheap modulo.
  }]=])
set(reference_delay_defined [=[  int read_index = fx_state_.delay_ptr - delay_duration + delay_line_size;
  while (read_index >= delay_line_size) {
    read_index -= delay_line_size;
  }
  int8_t* read_ptr = &fx_state_.delay_line[read_index];]=])
string(FIND "${reference_fx}" "${reference_delay}" reference_delay_position)
if(reference_delay_position LESS 0)
    message(FATAL_ERROR "Supported firmware oracle delay anchor changed")
endif()
string(REPLACE "${reference_delay}" "${reference_delay_defined}" reference_fx "${reference_fx}")
set(reference_pitch [=[    read_ptr += half_buffer_duration;
    if (read_ptr >= end) {
      read_ptr -= buffer_duration;
    } ]=])
set(reference_pitch_defined [=[    int second_index = fx_state_.ps_phase.integral + half_buffer_duration;
    if (second_index >= buffer_duration) {
      second_index -= buffer_duration;
    }
    read_ptr = &fx_state_.delay_line[second_index]; ]=])
string(FIND "${reference_fx}" "${reference_pitch}" reference_pitch_position)
if(reference_pitch_position LESS 0)
    message(FATAL_ERROR "Supported firmware oracle pitch anchor changed")
endif()
string(REPLACE "${reference_pitch}" "${reference_pitch_defined}" reference_fx "${reference_fx}")
set(reference_fx_overlay "${CMAKE_CURRENT_BINARY_DIR}/reference/fx_engine.cc")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/reference")
if(EXISTS "${reference_fx_overlay}")
    file(READ "${reference_fx_overlay}" previous_reference_fx)
endif()
if(NOT previous_reference_fx STREQUAL reference_fx)
    file(WRITE "${reference_fx_overlay}" "${reference_fx}")
endif()
add_library(SwaraBoardReference STATIC
    "${reference_fx_overlay}"
    "${SWARAXT_SHRUTHI_ROOT}/dsp/resources.cc")
target_include_directories(SwaraBoardReference PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Reference" "${SWARAXT_INCLUDE_FIRST}"
    "${SWARAXT_SHRUTHI_ROOT}" "${CMAKE_SOURCE_DIR}/Source")
target_compile_options(SwaraBoardReference PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/FI${SWARAXT_INCLUDE_FIRST}/swaraxt/shruthi_host.h>")

add_executable(SwaraBoardAlgorithmTests AlgorithmTests.cpp)
target_link_libraries(SwaraBoardAlgorithmTests PRIVATE SwaraBoardAlgorithm SwaraBoardReference)
swaraxt_enable_warnings(SwaraBoardAlgorithmTests)
add_test(NAME SwaraBoardAlgorithmTests COMMAND SwaraBoardAlgorithmTests)

add_executable(SwaraBoardModelTests ModelTests.cpp)
target_link_libraries(SwaraBoardModelTests PRIVATE SwaraBoardAlgorithm)
swaraxt_enable_warnings(SwaraBoardModelTests)
add_test(NAME SwaraBoardModelTests COMMAND SwaraBoardModelTests)

add_executable(SwaraXTDspFxNativeExactParityTests
    "${CMAKE_SOURCE_DIR}/Tests/Parity/DspFxNativeExactParityTestsMain.cpp")
target_include_directories(SwaraXTDspFxNativeExactParityTests PRIVATE "${CMAKE_SOURCE_DIR}/Tests")
target_link_libraries(SwaraXTDspFxNativeExactParityTests PRIVATE SwaraBoardAlgorithm)
swaraxt_enable_warnings(SwaraXTDspFxNativeExactParityTests)
add_test(NAME SwaraXTDspFxNativeExactParityTests COMMAND SwaraXTDspFxNativeExactParityTests)

add_executable(SwaraXTBoardPostDacFilterTests BoardPostDacFilterTests.cpp)
target_include_directories(SwaraXTBoardPostDacFilterTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraXTBoardPostDacFilterTests)
add_test(NAME SwaraXTBoardPostDacFilterTests COMMAND SwaraXTBoardPostDacFilterTests)

add_executable(SwaraXTPostDacFilterTests PostDacFilterTests.cpp)
target_include_directories(SwaraXTPostDacFilterTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
swaraxt_enable_warnings(SwaraXTPostDacFilterTests)
add_test(NAME SwaraXTPostDacFilterTests COMMAND SwaraXTPostDacFilterTests)

add_executable(SwaraXTFxEnumDispatchTests FxEnumDispatchTests.cpp)
target_link_libraries(SwaraXTFxEnumDispatchTests PRIVATE SwaraBoardAlgorithm)
swaraxt_enable_warnings(SwaraXTFxEnumDispatchTests)
add_test(NAME SwaraXTFxEnumDispatchTests COMMAND SwaraXTFxEnumDispatchTests)

add_executable(SwaraBoardSafetyTests SafetyTests.cpp
    "${CMAKE_SOURCE_DIR}/Source/Engine/SampleRate/HostResampler.cpp")
target_link_libraries(SwaraBoardSafetyTests PRIVATE SwaraBoardAlgorithm)
swaraxt_enable_warnings(SwaraBoardSafetyTests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME SwaraBoardSafetyTests COMMAND SwaraBoardSafetyTests --quick)
    set_tests_properties(SwaraBoardSafetyTests PROPERTIES TIMEOUT 300 LABELS quick)
    add_test(NAME SwaraBoardSafetyExtended COMMAND SwaraBoardSafetyTests --extended)
    set_tests_properties(SwaraBoardSafetyExtended PROPERTIES TIMEOUT 7200 LABELS extended)
else()
    add_test(NAME SwaraBoardSafetyTests COMMAND SwaraBoardSafetyTests)
endif()

swaraxt_add_plugin_console_test(SwaraBoardIntegrationTests IntegrationTests.cpp)
target_compile_definitions(SwaraBoardIntegrationTests PRIVATE SWARAXT_ENABLE_IDLE_CPU_TESTS=1)
swaraxt_add_plugin_console_test(SwaraBoardGuiTests GuiTests.cpp)

if(MSVC)
    swaraxt_add_plugin_console_test(SwaraBoardIntegrationSanitizerTests IntegrationTests.cpp)
    target_compile_definitions(SwaraBoardIntegrationSanitizerTests PRIVATE SWARAXT_ENABLE_IDLE_CPU_TESTS=1)
    target_compile_options(SwaraBoardIntegrationSanitizerTests PRIVATE /fsanitize=address /Zi)
    target_link_options(SwaraBoardIntegrationSanitizerTests PRIVATE /INCREMENTAL:NO /DEBUG)
    add_executable(SwaraBoardSanitizerTests ModelTests.cpp
        "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardResources.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardFilter.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardEffects.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardProcessor.cpp")
    target_include_directories(SwaraBoardSanitizerTests PRIVATE "${CMAKE_SOURCE_DIR}/Source")
    target_compile_options(SwaraBoardSanitizerTests PRIVATE /fsanitize=address /Zi)
    target_link_options(SwaraBoardSanitizerTests PRIVATE /INCREMENTAL:NO /DEBUG)
    swaraxt_enable_warnings(SwaraBoardSanitizerTests)
    add_test(NAME SwaraBoardSanitizerTests COMMAND SwaraBoardSanitizerTests)
    get_filename_component(swara_board_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(SwaraBoardSanitizerTests SwaraBoardIntegrationSanitizerTests PROPERTIES
        LABELS sanitizer TIMEOUT 600
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${swara_board_compiler_directory}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    swaraxt_label_tests()
endif()
