add_executable(OpdsParserTest
  OpdsParserTest.cpp
  OpenSearchDescriptionTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpenSearchDescription.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(OpdsParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
)

target_compile_definitions(OpdsParserTest PRIVATE
  OPDS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(OpdsParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(OpdsParserTest)
