add_executable(GlyphRunTest
  GlyphRunTest.cpp
)

target_include_directories(GlyphRunTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(GlyphRunTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphRunTest)
