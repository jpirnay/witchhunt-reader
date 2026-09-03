cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# On 32-bit x86 hosts (e.g. MinGW i686) GCC defaults to -mfpmath=387. The x87 FPU
# evaluates float expressions at 80-bit extended precision and rounds only on
# store, so a result that is exactly 460.0 under SSE can be 459.99999... here --
# and layout truncates rather than rounds, e.g. (int)(dims.width * scale) in
# ChapterHtmlSlimParser. That turns into off-by-one image dimensions and word
# positions, which makes the golden dumps fail locally while CI (x86-64, SSE2)
# passes. Pin the host tests to SSE2 so they reproduce CI bit-for-bit.
# Firmware targets RISC-V and is unaffected either way.
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(i.86|x86|AMD64)")
  add_compile_options(-msse2 -mfpmath=sse)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

# On MinGW/MSYS2, statically link the C++/GCC runtimes into each test exe.
# Otherwise the binary resolves libstdc++-6.dll / libgcc_s_seh-1.dll from PATH
# at runtime, and an unrelated copy (e.g. Git-for-Windows' mingw64, or the
# MINGW64/MSVCRT build when the exe was compiled with the UCRT64 toolchain)
# gets loaded. The std::string / iostream ABI mismatch then corrupts memory
# and segfaults deep inside libstdc++ on the file/string-heavy tests
# (hyphenation_eval, epub_benchmark). Static linkage removes the ambiguity.
if(MINGW)
  target_link_options(crosspoint_test_common INTERFACE
    -static-libstdc++
    -static-libgcc
    -static
  )
endif()

enable_testing()
include(GoogleTest)

add_subdirectory(${REPO_ROOT}/lib/SaxParser ${CMAKE_CURRENT_BINARY_DIR}/SaxParser)

add_subdirectory(buffered_io)
add_subdirectory(natural_sort)
add_subdirectory(firmware_board_tag)
add_subdirectory(list_layout)
add_subdirectory(cover_grid_layout)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(url_utils)
add_subdirectory(sax_parser)
add_subdirectory(opds_parser)
add_subdirectory(opds_format_label)
add_subdirectory(md_parser)
add_subdirectory(epub_benchmark)
add_subdirectory(footnote_shape)
add_subdirectory(css_gaps)
add_subdirectory(epub_css)
add_subdirectory(epub_opf)
add_subdirectory(layout_quality)
add_subdirectory(boot_diag_ring)
add_subdirectory(memory_model)
add_subdirectory(float_layout)
add_subdirectory(word_size_layout)
add_subdirectory(text_block_range)
add_subdirectory(zip_entry_reader)
# After zip_entry_reader: reuses its inflate_reader_host target.
add_subdirectory(png_decoder)
# After zip_entry_reader: reuses its inflate_reader_host target.
add_subdirectory(book_metadata_cache)
add_subdirectory(uzlib_lzcopy)
add_subdirectory(font_group_stream)
add_subdirectory(gif_decoder)
add_subdirectory(jpeg_decoder)
add_subdirectory(serial_transfer)
add_subdirectory(opds_filename)
add_subdirectory(epub_pipeline)
add_subdirectory(memory_arena)
add_subdirectory(content_bin)
add_subdirectory(touch_transform)
add_subdirectory(button_hint_strip)
add_subdirectory(list_touch_band)
add_subdirectory(tap_targets)
add_subdirectory(tap_zones)
add_subdirectory(font_size_ladder)
add_subdirectory(slider_geometry)
add_subdirectory(list_row_tap)
add_subdirectory(default_ignorable)
add_subdirectory(adaptive_tone)
add_subdirectory(gray8_writer)
add_subdirectory(glyph_fallback)
# After zip_entry_reader: reuses its inflate_reader_host target and HalStorage shim.
add_subdirectory(dictionary)
