add_executable(LibraryIndexTest
  LibraryKeysTest.cpp
  LibraryPublishTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBlob.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexReader.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryKeys.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryPublish.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(LibraryIndexTest PRIVATE
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(LibraryIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryIndexTest)
