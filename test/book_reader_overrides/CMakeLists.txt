add_executable(BookReaderOverridesTest
  BookReaderOverridesTest.cpp
  ${REPO_ROOT}/src/BookReaderOverrides.cpp
)

target_include_directories(BookReaderOverridesTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  # The stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(BookReaderOverridesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookReaderOverridesTest)
