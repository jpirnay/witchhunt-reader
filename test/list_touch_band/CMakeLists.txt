add_executable(ListTouchBandTest
  ListTouchBandTest.cpp
)

target_include_directories(ListTouchBandTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ListTouchBandTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListTouchBandTest)
