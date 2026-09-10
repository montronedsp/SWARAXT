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
set(SWARAXT_SHRUTHI_RESOURCE_SOURCE "${CMAKE_BINARY_DIR}/generated/shruthi_rom_resources.cc")
file(CONFIGURE OUTPUT "${SWARAXT_SHRUTHI_RESOURCE_SOURCE}"
    CONTENT "#include \"shruthi/rom_resource_extensions.h\"\n${shruthi_resources}" @ONLY)
unset(shruthi_resources)
