add_executable(ChapterBackfillTest
  ChapterBackfillTest.cpp
)

target_include_directories(ChapterBackfillTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ChapterBackfillTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterBackfillTest)
