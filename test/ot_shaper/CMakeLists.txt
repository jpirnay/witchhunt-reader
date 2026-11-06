add_executable(OtShaperTest
  OtShaperTest.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(OtShaperTest PRIVATE ${OT_SHAPER_INCLUDE_DIR})
# The four Indic fixture books (test/epubs) and their subset fonts: the plan's memos are checked on their words.
target_compile_definitions(OtShaperTest PRIVATE SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data"
                                               TTF_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/fonts"
                                               EPUB_FIXTURE_DIR="${REPO_ROOT}/test/epubs")
# inflate_reader_host (test/zip_entry_reader, added before this directory) inflates the books' chapters.
target_link_libraries(OtShaperTest PRIVATE crosspoint_test_common inflate_reader_host GTest::gtest_main)
gtest_discover_tests(OtShaperTest)

# Not a test: the command-line shaper compare_harfbuzz.py diffs against HarfBuzz.
add_executable(ot_shape_cli
  OtShapeCli.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(ot_shape_cli PRIVATE ${OT_SHAPER_INCLUDE_DIR} ${REPO_ROOT}/lib/Utf8)
