add_executable(InputFirstTest
  InputFirstTest.cpp
)

target_include_directories(InputFirstTest PRIVATE
  ${REPO_ROOT}/src
  # Arduino.h: millis() and host_clock::Ticking, for the sliced build.
  ${REPO_ROOT}/test/shims
)

target_link_libraries(InputFirstTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InputFirstTest)
