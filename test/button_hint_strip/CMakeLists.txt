add_executable(ButtonHintStripTest
  ButtonHintStripTest.cpp
)

target_include_directories(ButtonHintStripTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ButtonHintStripTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ButtonHintStripTest)
