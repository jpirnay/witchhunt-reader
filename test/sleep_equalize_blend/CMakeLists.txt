add_executable(SleepEqualizeBlendTest
  SleepEqualizeBlendTest.cpp
)

target_include_directories(SleepEqualizeBlendTest PRIVATE
  ${REPO_ROOT}/src
  # AdaptiveTone.h
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(SleepEqualizeBlendTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepEqualizeBlendTest)
