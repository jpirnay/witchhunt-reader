add_executable(ReaderMarginsTest
  ReaderMarginsTest.cpp
)

target_include_directories(ReaderMarginsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
)

target_link_libraries(ReaderMarginsTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ReaderMarginsTest)
