add_executable(BookmarksTest
  BookmarksTest.cpp
  ${REPO_ROOT}/src/GlobalBookmarkIndex.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
)

target_include_directories(BookmarksTest PRIVATE
  # The in-memory card; shadows the test/shims stub.
  ${REPO_ROOT}/test/file_replace
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
  # esp_heap_caps.h with a settable largest block.
  ${REPO_ROOT}/test/epub_pipeline
)

target_link_libraries(BookmarksTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookmarksTest)
