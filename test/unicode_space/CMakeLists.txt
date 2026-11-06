add_executable(UnicodeSpaceTest
  UnicodeSpaceTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${BIDI_MARK_SOURCES}
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(UnicodeSpaceTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/builtinFonts
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(UnicodeSpaceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UnicodeSpaceTest)
