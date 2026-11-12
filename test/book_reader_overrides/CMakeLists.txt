add_executable(BookReaderOverridesTest
  BookReaderOverridesTest.cpp
  BookReaderOverridesFileTest.cpp
  ${REPO_ROOT}/src/BookReaderOverrides.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(BookReaderOverridesTest PRIVATE
  # stdio-backed HalStorage.h, which CrossPointSettings.h also pulls in; must shadow the test/shims stub.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

# Inside the build tree, so two checkouts testing at once do not share a folder in /tmp.
target_compile_definitions(BookReaderOverridesTest PRIVATE BOOK_OVERRIDES_ROOT="${CMAKE_CURRENT_BINARY_DIR}/books")

target_link_libraries(BookReaderOverridesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookReaderOverridesTest)
