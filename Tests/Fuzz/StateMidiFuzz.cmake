# Linux LibFuzzer driver; no unbounded CTest registration.
juce_add_console_app(SwaraXTStateMidiFuzz PRODUCT_NAME "SwaraXTStateMidiFuzz")
juce_generate_juce_header(SwaraXTStateMidiFuzz)
target_sources(SwaraXTStateMidiFuzz PRIVATE Fuzz/StateMidiFuzz.cpp ${SWARAXT_ALL_PLUGIN_SOURCES})
target_compile_definitions(SwaraXTStateMidiFuzz PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SwaraXTStateMidiFuzz PRIVATE SwaraXTFonts juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
swaraxt_enable_warnings(SwaraXTStateMidiFuzz)
swaraxt_configure_shruthi_target(SwaraXTStateMidiFuzz)
swaraxt_enable_avrlib_hd_voice(SwaraXTStateMidiFuzz)
add_dependencies(SwaraXTStateMidiFuzz SwaraXtBuiltinTunings)
set_property(GLOBAL APPEND PROPERTY SWARAXT_CONSOLE_TEST_TARGETS SwaraXTStateMidiFuzz)
# The driver changes no production preprocessor definitions. Instrumentation
# is applied to every test/production TU by swaraxt_qualify_directory.
target_link_options(SwaraXTStateMidiFuzz PRIVATE -fsanitize=fuzzer)
