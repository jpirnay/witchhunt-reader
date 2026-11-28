add_executable(BleRadioPolicyTest
  BleRadioPolicyTest.cpp
)

target_include_directories(BleRadioPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BleRadioPolicyTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(BleRadioPolicyTest)
