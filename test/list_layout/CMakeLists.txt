add_executable(ListLayoutTest
  ListLayoutTest.cpp
  ${REPO_ROOT}/src/components/ListLayout.cpp
)

target_include_directories(ListLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListLayoutTest)
