enable_language(C)

# tinflate.c only; the ring contract is a property of the DEFLATE decoder plus the generated
# font tables, so this test deliberately avoids linking FontDecompressor (and with it Arduino,
# Logging and BuildArena) to stay a fast, dependency-light check of the format itself.
add_library(uzlib_font_ring STATIC ${REPO_ROOT}/lib/uzlib/src/tinflate.c)
target_include_directories(uzlib_font_ring PUBLIC ${REPO_ROOT}/lib/uzlib/src)

add_executable(FontGroupStreamTest
  FontGroupStreamTest.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
)
target_include_directories(FontGroupStreamTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/builtinFonts
)
target_link_libraries(FontGroupStreamTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(FontGroupStreamTest)
