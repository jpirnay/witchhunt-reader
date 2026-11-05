add_executable(WordSizeLayoutTest
  WordSizeLayoutTest.cpp
  WordFontSlotTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/TypographyProfile.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerticalDraw.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerticalOrientation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${BIDI_LAYOUT_SOURCES}
)

# Include order matters: this directory's GfxRenderer.h shim must win over the
# real lib/GfxRenderer (which drags in HalDisplay); lib/GfxRenderer is therefore
# listed last, for GlyphRun.h only.
target_include_directories(WordSizeLayoutTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${BIDI_LAYOUT_INCLUDES}
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(WordSizeLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordSizeLayoutTest)
