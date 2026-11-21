add_executable(GrayLevelPlanTest
  GrayLevelPlanTest.cpp
)

target_include_directories(GrayLevelPlanTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(GrayLevelPlanTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GrayLevelPlanTest)
