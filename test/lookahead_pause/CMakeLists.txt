add_executable(LookAheadPauseTest
  LookAheadPauseTest.cpp
)

target_include_directories(LookAheadPauseTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LookAheadPauseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LookAheadPauseTest)
