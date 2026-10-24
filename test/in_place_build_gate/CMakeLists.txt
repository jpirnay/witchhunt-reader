add_executable(InPlaceBuildGateTest
  InPlaceBuildGateTest.cpp
)

target_include_directories(InPlaceBuildGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(InPlaceBuildGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InPlaceBuildGateTest)
