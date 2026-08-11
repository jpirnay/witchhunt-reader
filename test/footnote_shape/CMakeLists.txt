add_executable(FootnoteShapeTest
  FootnoteShapeTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FootnoteShape.cpp
)

target_include_directories(FootnoteShapeTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(FootnoteShapeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FootnoteShapeTest)
