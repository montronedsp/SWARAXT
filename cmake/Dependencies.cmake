# SwaraXT — third-party dependencies at the caller-supplied path.

set(SWARAXT_JUCE_DIR "${CMAKE_SOURCE_DIR}/.cache/JUCE" CACHE PATH
    "Path to the JUCE source root")

if(NOT EXISTS "${SWARAXT_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${SWARAXT_JUCE_DIR}. Set SWARAXT_JUCE_DIR to a supported checkout")
endif()

if(APPLE)
    set(swaraxt_juce_version_regex "project\\(JUCE VERSION 9\\.0\\.1")
    set(swaraxt_juce_version_name "9.0.1")
else()
    set(swaraxt_juce_version_regex "project\\(JUCE VERSION 7\\.0\\.12")
    set(swaraxt_juce_version_name "7.0.12")
endif()

file(STRINGS "${SWARAXT_JUCE_DIR}/CMakeLists.txt" swaraxt_juce_project_line
     REGEX "${swaraxt_juce_version_regex}")
if(NOT swaraxt_juce_project_line)
    message(FATAL_ERROR "SWARAXT_JUCE_DIR must point to JUCE ${swaraxt_juce_version_name}")
endif()

set(JUCE_WEB_BROWSER 0 CACHE BOOL "Disable JUCE WebBrowser module" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${SWARAXT_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
