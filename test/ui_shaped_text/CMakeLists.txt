enable_language(C)

# UI text with its Indic and Myanmar runs shaped (lib/GfxRenderer/UiShapedText): a built-in UI face behind the shared
# FallbackFaceProvider, the reader's face a test double over the shaping fixtures
# (test/fixtures/fonts), shaped through IndicShaper and the OpenType engine. FontDecompressor for the provider's
# compressed faces (none here, but linked), with font_group_stream's uzlib_font_ring and Arduino.h.
add_executable(UiShapedTextTest
  UiShapedTextTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/UiShapedText.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/EpdFont/FallbackFaceProvider.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/ShapingFace.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/TextShaping/TextShaping.cpp
  ${REPO_ROOT}/lib/TextShaping/HebrewShaper.cpp
  ${REPO_ROOT}/lib/TextShaping/IndicShaper.cpp
  ${REPO_ROOT}/lib/Utf8/IndicReorder.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${BIDI_MARK_SOURCES}
  ${OT_SHAPER_SOURCES}
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
  # the heap counters and refusals; it replaces operator new and delete for the whole binary
  ${REPO_ROOT}/test/book_metadata_cache/AllocationRefusal.cpp
)

target_include_directories(UiShapedTextTest PRIVATE
  ${BIDI_MARK_INCLUDES}
  ${REPO_ROOT}/test/font_group_stream
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/text_shaping  # FixtureShapingFace.h
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/TextShaping
  ${REPO_ROOT}/lib/Utf8
  ${OT_SHAPER_INCLUDE_DIR}
)

target_compile_definitions(UiShapedTextTest PRIVATE
  TTF_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/fonts"
)

target_link_libraries(UiShapedTextTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)

gtest_discover_tests(UiShapedTextTest)
