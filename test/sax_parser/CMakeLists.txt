add_executable(SaxParserTest
  SaxParserTest.cpp
)

target_include_directories(SaxParserTest PRIVATE
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SaxParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(SaxParserTest)
