add_executable(CoverGridLayoutTest
  CoverGridLayoutTest.cpp
  ${REPO_ROOT}/src/components/CoverGridLayout.cpp
)

target_include_directories(CoverGridLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CoverGridLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverGridLayoutTest)
