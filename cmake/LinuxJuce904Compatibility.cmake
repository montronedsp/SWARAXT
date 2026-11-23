# Keep the base dependency pristine; this two-line compatibility overlay is build-owned.
set(swaraxt_channel_module "${SWARAXT_JUCE_DIR}/modules/juce_audio_basics")
set(swaraxt_channel_root "${CMAKE_BINARY_DIR}/generated/linux-juce904-compatibility/modules")
file(MAKE_DIRECTORY "${swaraxt_channel_root}")
file(COPY "${swaraxt_channel_module}" DESTINATION "${swaraxt_channel_root}"
    PATTERN "juce_AudioChannelSet.h" EXCLUDE
    PATTERN "juce_AudioChannelSet.cpp" EXCLUDE)
set(swaraxt_channel_header "${swaraxt_channel_root}/juce_audio_basics/buffers/juce_AudioChannelSet.h")
file(READ "${swaraxt_channel_module}/buffers/juce_AudioChannelSet.h" swaraxt_channel_text)
string(REPLACE "enum ChannelType" "enum ChannelType : unsigned int" swaraxt_channel_text "${swaraxt_channel_text}")
# CMake file(READ) normalizes CRLF. Restore the exact pinned header bytes.
string(REPLACE "\n" "\r\n" swaraxt_channel_text "${swaraxt_channel_text}")
set(swaraxt_existing_channel_hash "")
if(EXISTS "${swaraxt_channel_header}")
    file(SHA256 "${swaraxt_channel_header}" swaraxt_existing_channel_hash)
endif()
if(NOT swaraxt_existing_channel_hash STREQUAL "5f1308b04dbaf4646ac420001068b2b90f41c0977a61fce5931ac6988ffa7675")
    file(WRITE "${swaraxt_channel_header}" "${swaraxt_channel_text}")
endif()
file(SHA256 "${swaraxt_channel_header}" swaraxt_channel_hash)
if(NOT swaraxt_channel_hash STREQUAL "5f1308b04dbaf4646ac420001068b2b90f41c0977a61fce5931ac6988ffa7675")
    message(FATAL_ERROR "Generated ChannelType overlay hash differs from the qualified policy")
endif()
# Preserve the signed empty-bitset sentinel after the enum's promotion changes.
set(swaraxt_channel_cpp "${swaraxt_channel_root}/juce_audio_basics/buffers/juce_AudioChannelSet.cpp")
file(READ "${swaraxt_channel_module}/buffers/juce_AudioChannelSet.cpp" swaraxt_channel_cpp_text)
string(REPLACE "if (channels.getHighestBit() > topRearRight)"
    "if (channels.getHighestBit() > static_cast<int> (topRearRight))"
    swaraxt_channel_cpp_text "${swaraxt_channel_cpp_text}")
string(REPLACE "\n" "\r\n" swaraxt_channel_cpp_text "${swaraxt_channel_cpp_text}")
set(swaraxt_existing_cpp_hash "")
if(EXISTS "${swaraxt_channel_cpp}")
    file(SHA256 "${swaraxt_channel_cpp}" swaraxt_existing_cpp_hash)
endif()
if(NOT swaraxt_existing_cpp_hash STREQUAL "5e08cda3ba54106b5a59912df85f5942d019bdfd1e531069c08debba75fe251d")
    file(WRITE "${swaraxt_channel_cpp}" "${swaraxt_channel_cpp_text}")
endif()
file(SHA256 "${swaraxt_channel_cpp}" swaraxt_channel_cpp_hash)
if(NOT swaraxt_channel_cpp_hash STREQUAL "5e08cda3ba54106b5a59912df85f5942d019bdfd1e531069c08debba75fe251d")
    message(FATAL_ERROR "Generated ChannelType WAVE-sentinel implementation hash differs")
endif()
include_directories(BEFORE "${swaraxt_channel_root}")
target_include_directories(juce_audio_basics BEFORE INTERFACE "${swaraxt_channel_root}")
get_target_property(swaraxt_channel_sources juce_audio_basics INTERFACE_SOURCES)
string(REPLACE "${swaraxt_channel_module}/juce_audio_basics.cpp"
    "${swaraxt_channel_root}/juce_audio_basics/juce_audio_basics.cpp"
    swaraxt_channel_sources "${swaraxt_channel_sources}")
set_property(TARGET juce_audio_basics PROPERTY INTERFACE_SOURCES "${swaraxt_channel_sources}")
file(WRITE "${CMAKE_BINARY_DIR}/generated/linux-juce904-compatibility/manifest.json"
    "{\n  \"base_revision\": \"3fb5cbc0340758596dac39f7c8de47d562b4896e\",\n  \"channel_header_sha256\": \"${swaraxt_channel_hash}\",\n  \"channel_cpp_sha256\": \"${swaraxt_channel_cpp_hash}\",\n  \"queue_policy\": \"upstream 9.0.4\",\n  \"lock_policy\": \"upstream 9.0.4\",\n  \"link_policy\": \"exclude only SwaraXT SharedCode archive\",\n  \"midi_player_reserve_bytes\": 2048\n}\n")

# Every explicit JUCE consumer target checks input identity before compilation.
# An ALL-only check would be bypassed by --target SwaraXT_VST3.
add_custom_target(SwaraXTVerifyLinuxJuce904
    COMMAND "${CMAKE_COMMAND}"
        "-DSWARAXT_VERIFY_JUCE904_SOURCE=${SWARAXT_JUCE_DIR}"
        "-DSWARAXT_VERIFY_CHANNEL_HEADER=${swaraxt_channel_header}"
        "-DSWARAXT_VERIFY_CHANNEL_CPP=${swaraxt_channel_cpp}"
        -P "${CMAKE_CURRENT_LIST_DIR}/LinuxJuce904Identity.cmake"
    VERBATIM)
add_dependencies(juce_core SwaraXTVerifyLinuxJuce904)
