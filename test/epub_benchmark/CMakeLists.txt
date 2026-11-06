add_executable(EpubParserBenchmark EXCLUDE_FROM_ALL
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
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(EpubParserBenchmark PRIVATE
  crosspoint_test_common
  GTest::gtest
  SaxParser
)

# A manual tool, not a test: it prints timings and heap peaks, and three of its four cases
# end in SUCCEED(), so it is deliberately not registered with ctest. Parser correctness is
# covered by the epub_pipeline goldens. Build and run it by hand from the repo root:
#   cmake --build build/test --target EpubParserBenchmark
#   FIXTURE_DIR=test/fixtures/moby-dick build/test/epub_benchmark/EpubParserBenchmark
