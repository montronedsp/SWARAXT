# Keep upstream resources intact. These two AVR reads intentionally cross the
# declared table boundary; desktop objects must contain the actual ROM bytes.
file(READ "${SWARAXT_SHRUTHI_ROOT}/shruthi/resources.cc" shruthi_resources)
string(REGEX REPLACE
    "const prog_uint8_t wav_res_vowel_data\\[\\] PROGMEM = \\{[^}]*\\};"
    "const prog_uint8_t wav_res_vowel_data[] PROGMEM = { SWARAXT_AVR_VOWEL_ROM_BYTES };"
    shruthi_resources "${shruthi_resources}")
string(REGEX REPLACE
    "const prog_uint16_t lut_res_env_portamento_increments\\[\\] PROGMEM = \\{[^}]*\\};"
    "const prog_uint16_t lut_res_env_portamento_increments[] PROGMEM = { SWARAXT_AVR_ENVELOPE_ROM_WORDS };"
    shruthi_resources "${shruthi_resources}")
if(NOT shruthi_resources MATCHES "SWARAXT_AVR_VOWEL_ROM_BYTES" OR
   NOT shruthi_resources MATCHES "SWARAXT_AVR_ENVELOPE_ROM_WORDS")
    message(FATAL_ERROR "Upstream Shruthi ROM resource declarations changed")
endif()

# Several upstream LUTs hold signed musical intervals inside unsigned AVR tables
# (scale detunes, groove offsets, formant slopes). On AVR those negative literals
# converted implicitly; hosted C++ treats the same initializer as narrowing.
# Restate each literal as an explicit conversion to the table's own element type:
# the stored two's-complement bit pattern is unchanged and now visible in source.
function(swaraxt_rewrite_rom_resource_casts content_var)
    set(content "${${content_var}}")
    set(rewritten "")
    set(cursor 0)
    string(LENGTH "${content}" content_length)
    while(cursor LESS content_length)
        string(SUBSTRING "${content}" ${cursor} -1 tail)
        if(NOT tail MATCHES "const prog_uint(8|16)_t [A-Za-z0-9_]+\\[\\] PROGMEM = \\{")
            break()
        endif()
        set(element_type "prog_uint${CMAKE_MATCH_1}_t")
        set(declaration "${CMAKE_MATCH_0}")
        string(LENGTH "${declaration}" declaration_length)
        string(FIND "${tail}" "${declaration}" declaration_offset)
        math(EXPR body_offset "${declaration_offset} + ${declaration_length}")
        string(SUBSTRING "${tail}" ${body_offset} -1 after_declaration)
        string(FIND "${after_declaration}" "};" terminator_offset)
        if(terminator_offset LESS 0)
            break()
        endif()
        string(SUBSTRING "${tail}" 0 ${declaration_offset} prefix)
        string(SUBSTRING "${after_declaration}" 0 ${terminator_offset} body)
        string(REGEX REPLACE "(^|[^A-Za-z0-9_.])-([0-9]+)"
            "\\1static_cast<${element_type}>(-\\2)" body "${body}")
        string(APPEND rewritten "${prefix}${declaration}${body}};")
        math(EXPR cursor "${cursor} + ${body_offset} + ${terminator_offset} + 2")
    endwhile()
    string(SUBSTRING "${content}" ${cursor} -1 remainder)
    string(APPEND rewritten "${remainder}")
    set(${content_var} "${rewritten}" PARENT_SCOPE)
endfunction()

swaraxt_rewrite_rom_resource_casts(shruthi_resources)

set(SWARAXT_SHRUTHI_RESOURCE_SOURCE "${CMAKE_BINARY_DIR}/generated/shruthi_rom_resources.cc")
file(CONFIGURE OUTPUT "${SWARAXT_SHRUTHI_RESOURCE_SOURCE}"
    CONTENT "#include \"shruthi/rom_resource_extensions.h\"\n${shruthi_resources}" @ONLY)
unset(shruthi_resources)
