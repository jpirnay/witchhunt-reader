add_executable(DecimalFormatTest
  DecimalFormatTest.cpp
  ${REPO_ROOT}/src/util/DecimalFormat.cpp
)

target_include_directories(DecimalFormatTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DecimalFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DecimalFormatTest)
