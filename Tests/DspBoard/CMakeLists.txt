add_library(SwaraBoardAlgorithm STATIC
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardResources.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardFilter.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Engine/DspBoard/BoardEffects.cpp")
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
