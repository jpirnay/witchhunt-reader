find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(EXPAT libexpat)
endif()
if(NOT EXPAT_FOUND)
  find_library(EXPAT_LIBRARIES NAMES expat)
  find_path(EXPAT_INCLUDE_DIRS expat.h)
endif()

add_executable(EpubParserBenchmark
  EpubParserBenchmark.cpp
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
  ${EXPAT_INCLUDE_DIRS}
)

target_compile_definitions(EpubParserBenchmark PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_link_libraries(EpubParserBenchmark PRIVATE
  crosspoint_test_common
  GTest::gtest
  ${EXPAT_LIBRARIES}
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
