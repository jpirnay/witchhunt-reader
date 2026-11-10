enable_language(C)

# See timezone/CMakeLists.txt: the generated string blob's header has to be marked GENERATED again here.
set_source_files_properties(${I18N_GENERATED_KEYS} PROPERTIES GENERATED TRUE)

# Every key label of every keyboard layout, drawn by the SDK's keyboard() at an X3's and an X4's width, measured in
# the UI faces as the device has them: the built-in faces, which carry the Arabic presentation forms.
add_executable(KeyboardLabelsTest
  KeyboardLabelsTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${BIDI_MARK_SOURCES}
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(KeyboardLabelsTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
  # KeyboardLayoutSet.h for the layouts' native names: I18n.h and the generated keys it includes.
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

add_dependencies(KeyboardLabelsTest i18n_generated)

target_link_libraries(KeyboardLabelsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KeyboardLabelsTest)
