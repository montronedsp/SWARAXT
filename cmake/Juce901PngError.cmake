# JUCE 9.0.1 returns from its libpng error callback after setjmp, and the
# default handler then longjmps into that dead frame. On MSVC that is a
# non-continuable stack failure instead of an invalid image. The build-owned
# copy longjmps to the buffer JUCE installed for the decode.
set(png_root "${CMAKE_BINARY_DIR}/generated/png-error")
set(png_modules "${png_root}/modules")
set(png_loader "juce_graphics/image_formats/juce_PNGLoader.cpp")
set(png_source "${SWARAXT_JUCE_DIR}/modules/${png_loader}")
set(png_generated "${png_modules}/${png_loader}")
file(SHA256 "${png_source}" png_input_hash)
set(png_manifest "${png_root}/manifest.txt")
set(previous_png "")
if(EXISTS "${png_manifest}")
    file(READ "${png_manifest}" previous_png)
endif()
if(NOT previous_png STREQUAL "${png_input_hash}\n" AND NOT previous_png STREQUAL "${png_input_hash}")
    file(MAKE_DIRECTORY "${png_modules}")
    file(COPY "${SWARAXT_JUCE_DIR}/modules/juce_graphics" DESTINATION "${png_modules}")
    file(READ "${png_generated}" png_text)
    set(png_original "       #ifdef PNG_SETJMP_SUPPORTED
        setjmp (png_jmpbuf (p));
       #else
        longjmp (*(jmp_buf*) p->error_ptr, 1);
       #endif")
    set(png_corrected "        longjmp (*(jmp_buf*) png_get_error_ptr (p), 1);")
    string(FIND "${png_text}" "${png_original}" png_found)
    if(png_found LESS 0)
        message(FATAL_ERROR "JUCE 9.0.1 PNG error callback was not found")
    endif()
    string(REPLACE "${png_original}" "${png_corrected}" png_text "${png_text}")
    file(WRITE "${png_generated}" "${png_text}")
    file(WRITE "${png_manifest}" "${png_input_hash}")
endif()
get_target_property(png_sources juce_graphics INTERFACE_SOURCES)
string(REPLACE "${SWARAXT_JUCE_DIR}/modules/juce_graphics/juce_graphics.cpp"
    "${png_modules}/juce_graphics/juce_graphics.cpp" png_patched "${png_sources}")
if(png_patched STREQUAL png_sources)
    message(FATAL_ERROR "JUCE 9.0.1 graphics source was not redirected")
endif()
set_property(TARGET juce_graphics PROPERTY INTERFACE_SOURCES "${png_patched}")
