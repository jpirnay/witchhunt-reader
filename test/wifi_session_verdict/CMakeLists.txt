add_executable(WifiSessionVerdictTest
  WifiSessionVerdictTest.cpp
)

target_include_directories(WifiSessionVerdictTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(WifiSessionVerdictTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WifiSessionVerdictTest)
