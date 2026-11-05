add_executable(TextShapingTest
  TextShapingTest.cpp
  IndicShaperTest.cpp
  ${REPO_ROOT}/lib/TextShaping/TextShaping.cpp
  ${REPO_ROOT}/lib/TextShaping/HebrewShaper.cpp
  ${REPO_ROOT}/lib/TextShaping/IndicShaper.cpp
  ${REPO_ROOT}/lib/Utf8/IndicReorder.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  # the Indic module shapes through a face: the engine and the face's plans
  ${OT_SHAPER_SOURCES}
  ${REPO_ROOT}/lib/EpdFont/ShapingFace.cpp
  # AllocationRefusal(bytes) and the heap counters; it replaces operator new and delete for the whole binary
  ${REPO_ROOT}/test/book_metadata_cache/AllocationRefusal.cpp
)

target_include_directories(TextShapingTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../minibidi_arabic/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/TextShaping
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${OT_SHAPER_INCLUDE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/test/ot_shaper_port  # OtShaperTestHelpers.h
  ${REPO_ROOT}/test/complex_shaper  # ExpectedShaping.h
  ${REPO_ROOT}/test/ttf_font        # ExpectedGlyphRuns.h
  ${REPO_ROOT}/lib/GfxRenderer      # last: only for GlyphRun.h, the records a shaped run is stored as
)

target_compile_definitions(TextShapingTest PRIVATE
  SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data"
  TTF_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/fonts"
)

target_link_libraries(TextShapingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextShapingTest)
