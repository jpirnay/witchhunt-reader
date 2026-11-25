add_executable(SidecarFilesTest
  SidecarFilesTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/SidecarFiles.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
)

target_include_directories(SidecarFilesTest PRIVATE
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  # stdio-backed HalStorage.h; shadows the test/shims stub, whose exists() is always false.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SidecarFilesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SidecarFilesTest)
