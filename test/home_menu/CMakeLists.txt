add_executable(HomeMenuTest
  HomeMenuTest.cpp
  ${REPO_ROOT}/src/activities/home/HomeMenu.cpp
)

# HomeMenu.h names StrIds, so it needs the generated I18nKeys.h; nothing here translates.
add_dependencies(HomeMenuTest i18n_generated)

target_include_directories(HomeMenuTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/lib
  # I18n.h stub, and the stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(HomeMenuTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeMenuTest)
