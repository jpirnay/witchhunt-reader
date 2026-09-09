add_executable(SettingsGroupingTest
  SettingsGroupingTest.cpp
)

target_include_directories(SettingsGroupingTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib
  # I18n.h stub, and the stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SettingsGroupingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsGroupingTest)
