add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ReadingStatsJsonTest.cpp
  ${REPO_ROOT}/src/ReadingStats.cpp
  ${REPO_ROOT}/src/ReadingStatsJson.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${REPO_ROOT}/src
  # The no-op HalStorage JsonSettingsIO.h and ReadingStats.cpp pull in. It must precede lib/hal:
  # the real HalStorage.h there wants SdFat.
  ${REPO_ROOT}/test/shims
  # HalClock.h, whose now()/isSynced() the test stubs.
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
)

# localDayIndexFromEpoch() calls localtime_r, which MinGW's <time.h> declares only on request.
if(MINGW)
  target_compile_definitions(ReadingStatsTest PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
endif()

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)

# The store against real files in a temporary directory: write, read back, rename, cache.
add_executable(ReadingStatsStoreTest
  ReadingStatsStoreTest.cpp
  ${REPO_ROOT}/src/ReadingStats.cpp
  ${REPO_ROOT}/src/ReadingStatsJson.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReadingStatsStoreTest PRIVATE
  ${REPO_ROOT}/src
  # HalStorage.h over stdio and std::filesystem. Must precede test/shims, whose HalStorage.h is the
  # no-op one, and lib/hal, whose HalStorage.h wants SdFat.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
)

if(MINGW)
  target_compile_definitions(ReadingStatsStoreTest PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
endif()

target_link_libraries(ReadingStatsStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsStoreTest)

# The layout of reading-stats.bin against real files in a temporary directory.
add_executable(ReadingStatsSlotFileTest
  ReadingStatsSlotFileTest.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
)

target_include_directories(ReadingStatsSlotFileTest PRIVATE
  ${REPO_ROOT}/src
  # HalStorage.h over stdio and std::filesystem. Must precede test/shims (no-op HalStorage.h) and
  # lib/hal (the real one wants SdFat).
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(ReadingStatsSlotFileTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsSlotFileTest)
