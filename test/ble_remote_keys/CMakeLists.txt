add_executable(BleRemoteKeysTest
  BleRemoteKeysTest.cpp
)

target_include_directories(BleRemoteKeysTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include
)

target_link_libraries(BleRemoteKeysTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(BleRemoteKeysTest)
