# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""CMake policy control: only instrumented CI modules relax runtime imports."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ModulePolicy(unittest.TestCase):
    def test_shipping_strictness_and_instrumented_runtime_policy(self):
        policy = Path(__file__).resolve().parent / 'sanitizer-policy.cmake'
        text = '''cmake_minimum_required(VERSION 3.22)
project(ModulePolicy NONE)
set(CMAKE_SYSTEM_NAME Linux)
add_library(SwaraXT_VST3 MODULE IMPORTED GLOBAL)
set_property(TARGET SwaraXT_VST3 PROPERTY LINK_OPTIONS "-Wl,--no-undefined;preserved-option")
add_library(juce_recommended_lto_flags INTERFACE)
set_property(TARGET juce_recommended_lto_flags PROPERTY INTERFACE_COMPILE_OPTIONS -flto)
set_property(TARGET juce_recommended_lto_flags PROPERTY INTERFACE_LINK_LIBRARIES -flto)
include("POLICY")
set(SWARAXT_CI_SANITIZER OFF)
swaraxt_ci_sanitizer_policy()
get_target_property(options SwaraXT_VST3 LINK_OPTIONS)
get_target_property(lto juce_recommended_lto_flags INTERFACE_LINK_LIBRARIES)
if(NOT "-Wl,--no-undefined" IN_LIST options OR NOT lto STREQUAL "-flto")
    message(FATAL_ERROR "Shipping link policy changed")
endif()
set(SWARAXT_CI_SANITIZER ON)
swaraxt_ci_sanitizer_policy()
get_target_property(options SwaraXT_VST3 LINK_OPTIONS)
if(NOT options STREQUAL "preserved-option")
    message(FATAL_ERROR "Instrumented policy removed an unrelated link option")
endif()
foreach(property INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
    get_target_property(value juce_recommended_lto_flags ${property})
    if(value)
        message(FATAL_ERROR "Shipping LTO leaked into instrumented profile")
    endif()
endforeach()
'''.replace('POLICY', policy.as_posix())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'CMakeLists.txt').write_text(text)
            result = subprocess.run(['cmake', '-S', root, '-B', root / 'build', '-G', 'Ninja'],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__': unittest.main()
