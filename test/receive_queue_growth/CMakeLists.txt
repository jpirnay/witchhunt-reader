add_executable(ReceiveQueueGrowthTest
  ReceiveQueueGrowthTest.cpp
)

target_include_directories(ReceiveQueueGrowthTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReceiveQueueGrowthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReceiveQueueGrowthTest)
