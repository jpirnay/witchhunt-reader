add_executable(ReaderImageModeTest
  ReaderImageModeTest.cpp
)

target_include_directories(ReaderImageModeTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  # The stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(ReaderImageModeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderImageModeTest)
