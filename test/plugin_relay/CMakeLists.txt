add_executable(PluginRelayTest
  PluginRelayTest.cpp
)

target_include_directories(PluginRelayTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PluginRelayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluginRelayTest)
