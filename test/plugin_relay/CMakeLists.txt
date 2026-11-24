# The relay's rules (src/network/PluginRelay.h), the plugin folders (src/util/PluginLocations.h) and the hosts a plugin
# may reach (src/network/PluginHosts.h, which reads URLs with lib/SecureNet's UrlTarget, as the HTTP client does).
add_executable(PluginRelayTest
  PluginRelayTest.cpp
  PluginHostsTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  ${REPO_ROOT}/src/network/PluginHosts.cpp
  ${REPO_ROOT}/lib/SecureNet/src/UrlTarget.cpp
)

target_include_directories(PluginRelayTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/SecureNet/include
)

target_link_libraries(PluginRelayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluginRelayTest)
