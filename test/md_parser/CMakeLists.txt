add_executable(MdParserTest
  MdParserTest.cpp
  ${REPO_ROOT}/lib/Md/MdParser.cpp
)

target_include_directories(MdParserTest PRIVATE
  ${REPO_ROOT}/lib/Md
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(MdParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MdParserTest)
