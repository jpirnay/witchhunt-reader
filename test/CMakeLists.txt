cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
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

add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(url_utils)
add_subdirectory(sax_parser)
add_subdirectory(opds_parser)
add_subdirectory(opds_format_label)
add_subdirectory(md_parser)
add_subdirectory(epub_benchmark)
add_subdirectory(css_gaps)
add_subdirectory(epub_css)
add_subdirectory(layout_quality)
add_subdirectory(memory_model)
add_subdirectory(float_layout)
add_subdirectory(zip_entry_reader)
