add_executable(PopupProbeTest
  PopupProbeTest.cpp
)

target_include_directories(PopupProbeTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PopupProbeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PopupProbeTest)
