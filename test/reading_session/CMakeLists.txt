add_executable(ReadingSessionTrackerTest
  ReadingSessionTrackerTest.cpp
  ${REPO_ROOT}/src/ReadingSessionTracker.cpp
)

target_include_directories(ReadingSessionTrackerTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(ReadingSessionTrackerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingSessionTrackerTest)
