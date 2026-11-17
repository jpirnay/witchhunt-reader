add_executable(BuildPageGrayTest
  BuildPageGrayTest.cpp
)

target_include_directories(BuildPageGrayTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BuildPageGrayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildPageGrayTest)
