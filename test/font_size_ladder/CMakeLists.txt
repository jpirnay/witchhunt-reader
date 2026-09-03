add_executable(FontSizeLadderTest
  FontSizeLadderTest.cpp
)

target_include_directories(FontSizeLadderTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
)

target_link_libraries(FontSizeLadderTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(FontSizeLadderTest)
