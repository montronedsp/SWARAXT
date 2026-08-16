# SwaraXT — third-party dependencies (JUCE 7.0.12 at the caller-supplied path)

set(SWARAXT_JUCE_DIR "${CMAKE_SOURCE_DIR}/.cache/JUCE" CACHE PATH
    "Path to the pinned JUCE 7.0.12 source root")

if(NOT EXISTS "${SWARAXT_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${SWARAXT_JUCE_DIR}. Set SWARAXT_JUCE_DIR to the pinned JUCE 7.0.12 checkout")
endif()

file(STRINGS "${SWARAXT_JUCE_DIR}/CMakeLists.txt" swaraxt_juce_project_line
     REGEX "project\\(JUCE VERSION 7\\.0\\.12")
if(NOT swaraxt_juce_project_line)
    message(FATAL_ERROR "SWARAXT_JUCE_DIR must point to JUCE 7.0.12")
endif()

set(JUCE_WEB_BROWSER 0 CACHE BOOL "Disable JUCE WebBrowser module" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${SWARAXT_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
