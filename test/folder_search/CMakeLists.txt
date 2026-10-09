add_executable(FolderSearchTest
  FolderSearchTest.cpp
  ${REPO_ROOT}/src/activities/home/FolderSearch.cpp
)

target_include_directories(FolderSearchTest PRIVATE
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src/activities/home
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(FolderSearchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FolderSearchTest)
