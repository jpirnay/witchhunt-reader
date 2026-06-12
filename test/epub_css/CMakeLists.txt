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
