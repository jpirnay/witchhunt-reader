# The layout of recent.bin against real files in a temporary directory.
add_executable(RecentBooksFileTest
  RecentBooksFileTest.cpp
  ${REPO_ROOT}/src/RecentBooksFile.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(RecentBooksFileTest PRIVATE
  # HalStorage.h over stdio and std::filesystem. Must precede test/shims (no-op HalStorage.h) and lib/hal (the real one
  # wants SdFat).
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(RecentBooksFileTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RecentBooksFileTest)
