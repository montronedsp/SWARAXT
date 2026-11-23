# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
# Windows CI counterpart of the existing compatible test-object sharing.
function(swaraxt_ci_share_windows_tests)
# Build JUCE once per compatible module configuration within this build tree.
# Application/engine TUs retain all original target-specific definitions.
# Libraries are never reused across build trees, compilers or instrumentation.
if(SWARAXT_SHARE_TEST_JUCE AND MSVC)
    get_property(console_targets GLOBAL PROPERTY SWARAXT_CONSOLE_TEST_TARGETS)
    foreach(target IN LISTS console_targets)
        get_target_property(definitions ${target} COMPILE_DEFINITIONS)
        set(modal 0)
        if("JUCE_MODAL_LOOPS_PERMITTED=1" IN_LIST definitions)
            set(modal 1)
        endif()
        get_target_property(module_options ${target} COMPILE_OPTIONS)
        if(NOT module_options)
            set(module_options "")
        endif()
        # Ordinary MSVC profiles also contain focused ASan targets. Their
        # module objects must retain the exact target instrumentation options.
        string(SHA256 module_signature "${modal}|${module_options}")
        string(SUBSTRING "${module_signature}" 0 12 module_signature)
        set(support SwaraXTTestJuce${modal}_${module_signature})
        if(NOT TARGET ${support})
            add_library(${support} STATIC)
            target_compile_definitions(${support} PRIVATE
                JUCE_STANDALONE_APPLICATION=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_MODAL_LOOPS_PERMITTED=${modal})
            target_compile_options(${support} PRIVATE ${module_options})
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
if(SWARAXT_SHARE_TEST_PRODUCTION AND SWARAXT_SHARE_TEST_JUCE AND MSVC)
    get_property(console_targets GLOBAL PROPERTY SWARAXT_CONSOLE_TEST_TARGETS)
    set(groups "")
    foreach(target IN LISTS console_targets)
        get_target_property(definitions ${target} COMPILE_DEFINITIONS)
        get_target_property(options ${target} COMPILE_OPTIONS)
        get_target_property(includes ${target} INCLUDE_DIRECTORIES)
        get_target_property(links ${target} LINK_LIBRARIES)
        # Only this generated include directory depends on executable name.
        list(REMOVE_ITEM includes "${CMAKE_BINARY_DIR}/Tests/${target}_artefacts/JuceLibraryCode")
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
            "${CMAKE_BINARY_DIR}/Tests/${first}_artefacts/JuceLibraryCode/JuceHeader.h")
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

endfunction()
