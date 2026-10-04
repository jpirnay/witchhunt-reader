add_executable(ListGrammarTest
  ListGrammarTest.cpp
  ${REPO_ROOT}/src/util/ListGrammar.cpp
)

target_include_directories(ListGrammarTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListGrammarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListGrammarTest)
