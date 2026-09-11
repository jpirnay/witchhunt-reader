add_executable(PathContainmentTest
  PathContainmentTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_include_directories(PathContainmentTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(PathContainmentTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PathContainmentTest)
