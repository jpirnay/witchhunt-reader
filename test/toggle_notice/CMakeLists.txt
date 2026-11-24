add_executable(ToggleNoticeGateTest
  ToggleNoticeGateTest.cpp
)

target_include_directories(ToggleNoticeGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ToggleNoticeGateTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ToggleNoticeGateTest)
