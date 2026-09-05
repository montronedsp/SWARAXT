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

# Only the test executable links the unmodified original firmware oracle.
add_library(SwaraBoardReference STATIC
    "${SWARAXT_SHRUTHI_ROOT}/dsp/fx_engine.cc"
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

if(MSVC)
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
    set_tests_properties(SwaraBoardSanitizerTests PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${swara_board_compiler_directory}")
endif()
