add_executable(ClockAuthorityTest
  ClockAuthorityTest.cpp
)

target_include_directories(ClockAuthorityTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(ClockAuthorityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClockAuthorityTest)
