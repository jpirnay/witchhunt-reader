add_executable(SettingsGroupingTest
  SettingsGroupingTest.cpp
)

# I18nKeys.h is generated into the build tree (see test/CMakeLists.txt): the real
# lib/I18n/I18n.h includes it, and it does not exist in a clean checkout.
add_dependencies(SettingsGroupingTest i18n_generated)

target_include_directories(SettingsGroupingTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
  ${I18N_GENERATED_DIR}
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
