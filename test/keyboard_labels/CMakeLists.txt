enable_language(C)

# See timezone/CMakeLists.txt: the generated string blob's header has to be marked GENERATED again here.
set_source_files_properties(${I18N_GENERATED_KEYS} PROPERTIES GENERATED TRUE)

# Every key label of every keyboard layout, drawn by the SDK's keyboard() at an X3's and an X4's width, measured in
# the UI faces as the device has them: the built-in faces with the Arabic UI pack behind them (the shipped .cpfont
# files through SdCardFont, over the stdio HalStorage, as UiScriptPackTest reads them).
add_executable(KeyboardLabelsTest
  KeyboardLabelsTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
  ${REPO_ROOT}/lib/EpdFont/UiFaceProvider.cpp
  ${REPO_ROOT}/lib/EpdFont/FallbackFaceProvider.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${BIDI_MARK_SOURCES}
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(KeyboardLabelsTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/test/zip_entry_reader  # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/test/font_group_stream
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
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

target_compile_definitions(KeyboardLabelsTest PRIVATE
  ARABIC_UI_DIR="${REPO_ROOT}/assets/sd-fonts/ArabicUI"
)

target_link_libraries(KeyboardLabelsTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)

gtest_discover_tests(KeyboardLabelsTest)
