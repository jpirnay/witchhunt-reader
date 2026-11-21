add_executable(ClockDriftTest
  ClockDriftTest.cpp
)

target_include_directories(ClockDriftTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(ClockDriftTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClockDriftTest)
