add_executable(BuildArenaTest
  BuildArenaTest.cpp
  PassTableTest.cpp
)

target_include_directories(BuildArenaTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  # The settable esp_heap_caps.h host stub PassTable sizes its heap tables from.
  ${REPO_ROOT}/test/epub_pipeline
)

target_link_libraries(BuildArenaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildArenaTest)

# The rewind fill exists only with BUILD_ARENA_TRACE, so it is tested in a binary of its own (see the test's header).
add_executable(BuildArenaRewindFillTest
  BuildArenaRewindFillTest.cpp
)
target_include_directories(BuildArenaRewindFillTest PRIVATE ${REPO_ROOT}/lib/Memory)
target_compile_definitions(BuildArenaRewindFillTest PRIVATE BUILD_ARENA_TRACE=1)
target_link_libraries(BuildArenaRewindFillTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BuildArenaRewindFillTest)
