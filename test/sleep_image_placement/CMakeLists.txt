add_executable(SleepImagePlacementTest
  SleepImagePlacementTest.cpp
)

target_include_directories(SleepImagePlacementTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(SleepImagePlacementTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepImagePlacementTest)
