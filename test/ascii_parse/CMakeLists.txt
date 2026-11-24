# The hex and dotted-quad parsers (src/util/AsciiParse.h), against the sscanf formats they replaced.
add_executable(AsciiParseTest
  AsciiParseTest.cpp
  ${REPO_ROOT}/src/util/AsciiParse.cpp
)

target_include_directories(AsciiParseTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(AsciiParseTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(AsciiParseTest)
