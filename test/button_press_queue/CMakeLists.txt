add_executable(ButtonPressQueueTest
  ButtonPressQueueTest.cpp
)

target_include_directories(ButtonPressQueueTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ButtonPressQueueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ButtonPressQueueTest)
