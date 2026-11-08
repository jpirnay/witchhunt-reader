add_executable(InPlaceBuildGateTest
  InPlaceBuildGateTest.cpp
)

target_include_directories(InPlaceBuildGateTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(InPlaceBuildGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InPlaceBuildGateTest)
