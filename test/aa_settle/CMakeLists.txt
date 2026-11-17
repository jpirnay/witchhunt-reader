add_executable(AaSettleTest
  AaSettleTest.cpp
)

target_include_directories(AaSettleTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(AaSettleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AaSettleTest)
