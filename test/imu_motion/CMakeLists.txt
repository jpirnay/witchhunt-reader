add_executable(ImuMotionTest
  OrientationTrackerTest.cpp
  TiltRateTest.cpp
  ${REPO_ROOT}/lib/hal/OrientationTracker.cpp
)

target_include_directories(ImuMotionTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(ImuMotionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImuMotionTest)
