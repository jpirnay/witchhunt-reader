add_executable(InlineListFunctionRefTest
  InlineListFunctionRefTest.cpp
)

target_include_directories(InlineListFunctionRefTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(InlineListFunctionRefTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InlineListFunctionRefTest)
