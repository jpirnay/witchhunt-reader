add_executable(LightSleepGateTest
  LightSleepGateTest.cpp
)

target_include_directories(LightSleepGateTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(LightSleepGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LightSleepGateTest)
