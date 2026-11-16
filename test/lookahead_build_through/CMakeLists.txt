add_executable(BuildThroughGateTest
  BuildThroughGateTest.cpp
)

target_include_directories(BuildThroughGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BuildThroughGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildThroughGateTest)
