add_executable(KeyboardGridTest
  KeyboardGridTest.cpp
)

target_include_directories(KeyboardGridTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(KeyboardGridTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KeyboardGridTest)
