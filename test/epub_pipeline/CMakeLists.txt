# Full-pipeline equivalence harness (docs/compiled-book-pipeline-plan.md Phase 0):
# builds the REAL Epub/Section/ParsedText/CssParser pipeline for the host against
# deterministic stubs (GfxRenderer metrics, ESP heap/wdt, I18n) and the real-I/O
# HalStorage shim from test/zip_entry_reader.
enable_language(C)

set(PIPELINE_SOURCES
  PipelineRunner.cpp
  LoggingImpl.cpp
  # lib/Epub — the code under test
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/CompiledContent.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/BlockSerialization.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/BlockStreamReader.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/ContentBinWriter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/ContentBinCompiler.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/ContentSink.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/LayoutSink.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/PageLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/FootnotePreviewLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/ImageLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/content/TableLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FootnotePreviews.cpp
  ${REPO_ROOT}/lib/Epub/Epub/EpubImageManifest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageListSink.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageMapParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp
  # supporting libs the pipeline links against
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/ProgressiveJpegDc/ProgressiveJpegDc.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/PngDecoder/PngStreamDecoder.cpp
  ${REPO_ROOT}/lib/TJpgDec/src/tjpgd.c
)

set(PIPELINE_INCLUDES
  # Our stubs must shadow the real device headers: GfxRenderer.h, HalDisplay.h,
  # I18n.h, esp_*.h.
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Real stdio-backed FsFile/Storage (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: Arduino.h, Print.h, HardwareSerial.h, WString.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  # For Bitmap.h/BitmapHelpers.h only — our stub dir precedes this, so the
  # real GfxRenderer.h in the same directory stays shadowed.
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/JpegToBmpConverter
  ${REPO_ROOT}/lib/PngToBmpConverter
  ${REPO_ROOT}/lib/PngDecoder
  ${REPO_ROOT}/lib/ProgressiveJpegDc
  ${REPO_ROOT}/lib/TJpgDec/src
)

add_executable(EpubPipelineTest
  EpubPipelineTest.cpp
  FingerprintTest.cpp
  Stage1ProducerTest.cpp
  ContentSinkTest.cpp
  ContentBinStreamTest.cpp
  ContentBinCompilerTest.cpp
  LayoutSinkTest.cpp
  PageLayoutTest.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(EpubPipelineTest PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(EpubPipelineTest PRIVATE
  CORPUS_DIR="${REPO_ROOT}/test/epubs"
  FIXTURES_DIR="${REPO_ROOT}/test/fixtures"
  GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
  EPUB_STAGE1=1
)
target_link_libraries(EpubPipelineTest PRIVATE
  crosspoint_test_common
  SaxParser
  GTest::gtest_main
)
gtest_discover_tests(EpubPipelineTest PROPERTIES TIMEOUT 300)

# HeapTrack overrides malloc/free process-wide — CLI tool only, never the gtest.
add_executable(epub_pipeline_dump
  DumpMain.cpp
  HeapTrack.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(epub_pipeline_dump PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(epub_pipeline_dump PRIVATE
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
)
target_link_libraries(epub_pipeline_dump PRIVATE
  crosspoint_test_common
  SaxParser
)

# Stage-1 content.bin dump: drives every spine of a book through one ContentSink,
# serializes content.bin, and prints a canonical text dump (Phase 3 step 4).
add_executable(content_stage1_dump
  Stage1DumpMain.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(content_stage1_dump PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(content_stage1_dump PRIVATE
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
  EPUB_STAGE1=1
)
target_link_libraries(content_stage1_dump PRIVATE
  crosspoint_test_common
  SaxParser
)
