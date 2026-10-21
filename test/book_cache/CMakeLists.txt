add_executable(BookCacheTest
  BookCacheTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/BookCache.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(BookCacheTest PRIVATE
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
)

# Inside the build tree, so two checkouts testing at once do not share a folder in /tmp.
target_compile_definitions(BookCacheTest PRIVATE BOOK_CACHE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/caches")

target_link_libraries(BookCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCacheTest)
