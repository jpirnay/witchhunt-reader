add_executable(ListRowTapTest
  ListRowTapTest.cpp
)

target_include_directories(ListRowTapTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListRowTapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListRowTapTest)
