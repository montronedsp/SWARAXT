# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Configure-time regression: instrumented and ordinary objects never mix."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class Sharing(unittest.TestCase):
    def test_msvc_module_options_and_production_groups_are_preserved(self):
        helper = Path(__file__).resolve().parent / 'windows-sharing.cmake'
        project = r'''
cmake_minimum_required(VERSION 3.29)
project(SharingContract LANGUAGES CXX)
# Exercise target-graph construction, not an emulated Windows compile.
set(MSVC TRUE)
set(SWARAXT_SHARE_TEST_JUCE ON)
set(SWARAXT_SHARE_TEST_PRODUCTION ON)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module.cpp" "int module(){return 0;}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/production.cpp" "int engine(){return 0;}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test.cpp" "int main(){return 0;}\n")
set(SWARAXT_ALL_PLUGIN_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/production.cpp")
foreach(name juce_audio_utils juce_dsp juce_recommended_config_flags juce_recommended_warning_flags)
    add_library(${name} INTERFACE)
    add_library(juce::${name} ALIAS ${name})
endforeach()
target_sources(juce_audio_utils INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/module.cpp")
add_custom_target(SwaraXtBuiltinTunings)
foreach(name OrdinaryA OrdinaryB Instrumented)
    set(header_dir "${CMAKE_BINARY_DIR}/Tests/${name}_artefacts/JuceLibraryCode")
    file(MAKE_DIRECTORY "${header_dir}")
    file(WRITE "${header_dir}/JuceHeader.h" "// fixture\n")
    add_executable(${name} "${CMAKE_CURRENT_BINARY_DIR}/test.cpp" ${SWARAXT_ALL_PLUGIN_SOURCES})
    target_include_directories(${name} PRIVATE "${header_dir}")
    target_compile_definitions(${name} PRIVATE JUCE_MODAL_LOOPS_PERMITTED=0)
    target_compile_options(${name} PRIVATE /W4 /permissive- /WX)
    target_link_libraries(${name} PRIVATE juce::juce_audio_utils juce::juce_dsp)
endforeach()
target_compile_options(Instrumented PRIVATE /fsanitize=address /Zi)
set_property(GLOBAL PROPERTY SWARAXT_CONSOLE_TEST_TARGETS OrdinaryA OrdinaryB Instrumented)
include("HELPER")
swaraxt_ci_share_windows_tests()
get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(module_count 0)
set(instrumented_count 0)
foreach(target IN LISTS targets)
    if(target MATCHES "^SwaraXTTestJuce")
        math(EXPR module_count "${module_count}+1")
        get_target_property(options ${target} COMPILE_OPTIONS)
        if("/fsanitize=address" IN_LIST options)
            math(EXPR instrumented_count "${instrumented_count}+1")
        endif()
        if(NOT "/WX" IN_LIST options)
            message(FATAL_ERROR "Warning policy dropped from shared module")
        endif()
    endif()
endforeach()
if(NOT module_count EQUAL 2 OR NOT instrumented_count EQUAL 1)
    message(FATAL_ERROR "Instrumented and ordinary module groups were not separated")
endif()
get_target_property(a OrdinaryA LINK_LIBRARIES)
get_target_property(b OrdinaryB LINK_LIBRARIES)
get_target_property(c Instrumented LINK_LIBRARIES)
if(NOT a STREQUAL b OR a STREQUAL c)
    message(FATAL_ERROR "Incompatible production objects were shared")
endif()
get_target_property(c_sources Instrumented SOURCES)
if(NOT "${CMAKE_CURRENT_BINARY_DIR}/production.cpp" IN_LIST c_sources)
    message(FATAL_ERROR "Singleton instrumented production source lost")
endif()
'''.replace('HELPER', helper.as_posix())
        with tempfile.TemporaryDirectory(prefix='swara sharing ') as directory:
            root = Path(directory); (root / 'CMakeLists.txt').write_text(project)
            result = subprocess.run(['cmake', '-S', root, '-B', root / 'build', '-G', 'Ninja'],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__': unittest.main()
