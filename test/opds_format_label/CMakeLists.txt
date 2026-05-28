add_executable(OpdsFormatLabelTest
  OpdsFormatLabelTest.cpp
  ${REPO_ROOT}/src/activities/browser/OpdsFormatLabel.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
)

target_include_directories(OpdsFormatLabelTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/OpdsParser
)

target_link_libraries(OpdsFormatLabelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(OpdsFormatLabelTest)
