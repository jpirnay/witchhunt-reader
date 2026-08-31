# Declaration-level parser tests. These also have a standalone driver in
# test/run_epub_css_test.sh (which compiles the real Logging.cpp and so needs the Arduino
# framework headers); building them here too means they run in the default ctest sweep
# on any host, which is where a regression would otherwise go unnoticed. Hand-rolled
# asserts and its own main(), so no gtest_main.
add_executable(CssParserTest
  CssParserTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

target_include_directories(CssParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(CssParserTest PRIVATE crosspoint_test_common)

add_test(NAME CssParser.Declarations COMMAND CssParserTest)

# Shims (HalStorage.h, LoggingStub.cpp with uzlib checksum stubs) are shared
# with the ZipEntryReaderTest harness — keep them in test/zip_entry_reader.
add_executable(EpubCssPerformanceTest
  CssParserPerfTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

target_include_directories(EpubCssPerformanceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(EpubCssPerformanceTest PRIVATE
  FIXTURE_EPUB="${REPO_ROOT}/test/fixtures/test_large_css.epub"
)

target_link_libraries(EpubCssPerformanceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubCssPerformanceTest)
