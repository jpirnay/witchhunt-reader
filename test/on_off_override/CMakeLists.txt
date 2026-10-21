add_executable(OnOffOverrideTest
  OnOffOverrideTest.cpp
)

target_include_directories(OnOffOverrideTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OnOffOverrideTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OnOffOverrideTest)
