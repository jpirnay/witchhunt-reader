add_executable(PluginRelayTest
  PluginRelayTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_include_directories(PluginRelayTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(PluginRelayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluginRelayTest)
