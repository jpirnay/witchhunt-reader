add_executable(DefaultIgnorableTest
  DefaultIgnorableTest.cpp
)

target_include_directories(DefaultIgnorableTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/builtinFonts
)

target_link_libraries(DefaultIgnorableTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DefaultIgnorableTest)
