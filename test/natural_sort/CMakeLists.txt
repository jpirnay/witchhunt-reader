add_executable(NaturalSortTest
  NaturalSortTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_include_directories(NaturalSortTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(NaturalSortTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NaturalSortTest)
