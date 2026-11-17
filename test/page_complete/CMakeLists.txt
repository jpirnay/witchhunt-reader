add_executable(PageCompleteTest
  PageCompleteTest.cpp
)

target_include_directories(PageCompleteTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PageCompleteTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageCompleteTest)
