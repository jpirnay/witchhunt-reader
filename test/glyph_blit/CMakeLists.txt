add_executable(GlyphBlitTest GlyphBlitTest.cpp)

# GlyphBlit.h is header-only and needs nothing from GfxRenderer.h.
target_include_directories(GlyphBlitTest PRIVATE ${REPO_ROOT}/lib/GfxRenderer)

target_link_libraries(GlyphBlitTest PRIVATE GTest::gtest_main)

gtest_discover_tests(GlyphBlitTest)
