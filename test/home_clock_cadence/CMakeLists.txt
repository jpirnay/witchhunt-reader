add_executable(HomeClockCadenceTest
  HomeClockCadenceTest.cpp
)

target_include_directories(HomeClockCadenceTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeClockCadenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeClockCadenceTest)
