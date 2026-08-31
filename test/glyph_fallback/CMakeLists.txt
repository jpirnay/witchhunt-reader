add_executable(GlyphFallbackTest
  GlyphFallbackTest.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
)

target_include_directories(GlyphFallbackTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(GlyphFallbackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphFallbackTest)
