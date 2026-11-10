enable_language(C)

# The CJK UI packs as the device reads them: the shipped .cpfont files through SdCardFont, over the stdio HalStorage,
# behind a built-in UI face through the CJK fallback (FallbackFaceProvider).
add_executable(CjkUiPackTest
  CjkUiPackTest.cpp
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

target_include_directories(CjkUiPackTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/test/zip_entry_reader  # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/test/font_group_stream
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(CjkUiPackTest PRIVATE
  SD_FONTS_DIR="${REPO_ROOT}/assets/sd-fonts"
)

target_link_libraries(CjkUiPackTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)

gtest_discover_tests(CjkUiPackTest)
