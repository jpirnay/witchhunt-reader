add_executable(WordShapeCacheTest
  WordShapeCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordShapeCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ShapedStore.cpp
  # AllocationRefusal(bytes) and the heap counters; it replaces operator new and delete for the whole binary
  ${REPO_ROOT}/test/book_metadata_cache/AllocationRefusal.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(WordShapeCacheTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/TextShaping  # IndicShaper.h: PieceSink, PieceKind
  ${REPO_ROOT}/lib/Memory       # Memory.h, BuildArena.h
  ${REPO_ROOT}/lib/Utf8         # IndicScripts.h
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer  # GlyphRun.h
  ${REPO_ROOT}/test/shims       # Arduino.h (ESP.getMaxAllocHeap), HardwareSerial.h
)

target_link_libraries(WordShapeCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordShapeCacheTest)
