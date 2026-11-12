add_executable(ScreenshotRequestTest
  ScreenshotRequestTest.cpp
)

target_include_directories(ScreenshotRequestTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ScreenshotRequestTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ScreenshotRequestTest)
