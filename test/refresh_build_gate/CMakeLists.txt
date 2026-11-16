add_executable(BuildDuringRefreshTest
  BuildDuringRefreshTest.cpp
)

target_include_directories(BuildDuringRefreshTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BuildDuringRefreshTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildDuringRefreshTest)
