# Which Greek and Hebrew characters the built-in UI faces hold (convert-builtin-fonts.sh UI_SCRIPT_INTERVALS).
add_executable(UiFaceScriptsTest
  UiFaceScriptsTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${BIDI_MARK_SOURCES}
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(UiFaceScriptsTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

target_link_libraries(UiFaceScriptsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UiFaceScriptsTest)
