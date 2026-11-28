# The I18n stub below links the generated English blob; see test/timezone/CMakeLists.txt for why
# the GENERATED property has to be set again in this directory.
set_source_files_properties(${I18N_GENERATED_STRINGS} PROPERTIES GENERATED TRUE)

add_executable(ButtonActionOptionsTest
  ButtonActionOptionsTest.cpp
  ${I18N_GENERATED_STRINGS}
)

add_dependencies(ButtonActionOptionsTest i18n_generated)

target_include_directories(ButtonActionOptionsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  # HalCapabilities.h (ButtonActionOptions::boardCaps), after the shims as in test/timezone.
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(ButtonActionOptionsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ButtonActionOptionsTest)
