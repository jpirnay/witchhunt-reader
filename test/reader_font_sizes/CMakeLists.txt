add_executable(ReaderFontSizesTest
  ReaderFontSizesTest.cpp
)

target_include_directories(ReaderFontSizesTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
)

target_link_libraries(ReaderFontSizesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderFontSizesTest)
