# An unsupported caller is a sensitivity control, never a supported-path PASS.
execute_process(COMMAND "${TEST_EXECUTABLE}" unreserved-output
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
message("Unsupported caller exit: ${result}\n${output}${error}")
set(report "${output}${error}")
if(NOT result MATCHES "^[1-9][0-9]*$" AND NOT result STREQUAL "Subprocess aborted")
    message(FATAL_ERROR "Capacity control did not terminate with the expected failure")
endif()
foreach(expected "RealtimeSanitizer: unsafe-library-call" "realloc" "MidiBuffer::addEvent" "flushSequencerMidi")
    if(NOT report MATCHES "${expected}")
        message(FATAL_ERROR "Capacity control did not detect expected ${expected}")
    endif()
endforeach()
