function(swaraxt_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Warnings-as-errors, enforced only where the tree is actually warning-free.
#
# MSVC /W4 /permissive- is clean, so /WX keeps it that way. GCC/Clang are not
# clean yet: -Wall -Wextra still report -Wunused-parameter from the untouched
# upstream Shruthi headers (midi/midi.h, shruthi/sequencer_settings.h) plus
# -Wcomment, -Wconversion-null and -Wunused-but-set-variable in the historical
# firmware sources. Those warnings are now visible instead of suppressed;
# turning them into errors would break the Linux and macOS builds outright.
function(swaraxt_enable_warnings_as_errors target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    endif()
endfunction()
