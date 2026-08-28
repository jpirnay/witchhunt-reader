add_executable(AdaptiveToneTest
  AdaptiveToneTest.cpp
)

target_include_directories(AdaptiveToneTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(AdaptiveToneTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AdaptiveToneTest)
