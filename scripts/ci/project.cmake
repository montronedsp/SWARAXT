# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
# Opt-in CI hook. Normal builds never include this file.
if(PROJECT_NAME STREQUAL "SwaraXT" AND PROJECT_IS_TOP_LEVEL)
    function(swaraxt_ci_finalize)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sanitizer-policy.cmake")
        swaraxt_ci_sanitizer_policy()
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows-sharing.cmake")
        if(MSVC AND SWARAXT_SHARE_TEST_JUCE AND SWARAXT_SHARE_TEST_PRODUCTION)
            swaraxt_ci_share_windows_tests()
        endif()
        juce_add_console_app(SwaraXTNativePluginHost PRODUCT_NAME "SwaraXTNativePluginHost")
        juce_generate_juce_header(SwaraXTNativePluginHost)
        target_sources(SwaraXTNativePluginHost PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PluginHost.cpp")
        target_compile_definitions(SwaraXTNativePluginHost PRIVATE JUCE_PLUGINHOST_VST3=1
            JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(SwaraXTNativePluginHost PRIVATE juce::juce_audio_utils
            juce::juce_recommended_config_flags)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Xlib's process-global locale cache must outlive its users and
            # remain rooted when JUCE unloads its dynamic X11 symbol table.
            target_link_libraries(SwaraXTNativePluginHost PRIVATE
                "-Wl,--no-as-needed" X11 "-Wl,--as-needed")
            target_link_options(SwaraXTNativePluginHost PRIVATE "-Wl,--wrap=pthread_mutex_destroy")
        endif()
        if(SWARAXT_CI_REFERENCE_ROOT)
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            if(CMAKE_VERSION VERSION_LESS 3.29)
                message(FATAL_ERROR "Independent native audio launchers require CMake 3.29+")
            endif()
            foreach(target SwaraXTAudioParityTests SwaraXTDualSvfSection2Tests
                           SwaraXTHostSrcExactParityTests SwaraXTDspFxIntegratedExactParityTests)
                set_property(TARGET ${target} PROPERTY TEST_LAUNCHER
                    "${Python3_EXECUTABLE};${CMAKE_CURRENT_FUNCTION_LIST_DIR}/native_reference.py;--reference-root;${SWARAXT_CI_REFERENCE_ROOT};--test-name;${target}")
            endforeach()
            add_test(NAME SwaraXTDesktopNativeReferenceVerifierTests COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/test_native_reference.py")
        endif()
    endfunction()
    cmake_language(DEFER CALL swaraxt_ci_finalize)
endif()
