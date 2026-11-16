add_executable(ImuMotionTest
  TiltRateTest.cpp
)

target_include_directories(ImuMotionTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(ImuMotionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImuMotionTest)
