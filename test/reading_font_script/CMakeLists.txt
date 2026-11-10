add_executable(ReadingFontScriptTest
  ReadingFontScriptTest.cpp
)

target_include_directories(ReadingFontScriptTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReadingFontScriptTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ReadingFontScriptTest)
