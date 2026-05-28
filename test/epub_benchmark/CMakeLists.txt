add_executable(EpubParserBenchmark
  EpubParserBenchmark.cpp
  stubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(EpubParserBenchmark PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(EpubParserBenchmark PRIVATE
  crosspoint_test_common
  GTest::gtest
  SaxParser
)

# Benchmark is registered as a CTest but we don't use gtest_discover_tests
# so it doesn't pollute the test list with timing-sensitive "tests".
# Run manually: ctest -R EpubParserBenchmark -V
add_test(NAME EpubParserBenchmark
  COMMAND EpubParserBenchmark
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(EpubParserBenchmark PROPERTIES
  ENVIRONMENT "FIXTURE_DIR=${REPO_ROOT}/test/fixtures/moby-dick"
)
