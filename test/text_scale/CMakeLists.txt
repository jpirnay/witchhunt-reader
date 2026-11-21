add_executable(TextScaleTest
  TextScaleTest.cpp
)

target_include_directories(TextScaleTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(TextScaleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextScaleTest)
