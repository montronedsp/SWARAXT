# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
function(swaraxt_ci_sanitizer_policy)
    if(NOT SWARAXT_CI_SANITIZER)
        return()
    endif()
    foreach(property INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES)
        set_property(TARGET juce_recommended_lto_flags PROPERTY ${property} "")
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Clang's instrumented ELF module imports sanitizer interfaces from the
        # already-instrumented host. Shipping Release links remain strict.
        # An RTLD_NOW probe and the actual matching host must resolve everything.
        get_target_property(options SwaraXT_VST3 LINK_OPTIONS)
        list(REMOVE_ITEM options "-Wl,--no-undefined")
        set_property(TARGET SwaraXT_VST3 PROPERTY LINK_OPTIONS "${options}")
    endif()
endfunction()
