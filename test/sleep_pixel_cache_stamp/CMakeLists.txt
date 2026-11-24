add_executable(SleepPixelCacheStampTest
  SleepPixelCacheStampTest.cpp
)

target_include_directories(SleepPixelCacheStampTest PRIVATE
  ${REPO_ROOT}/src/activities/boot_sleep
  # HalStorage.h: the in-memory HalFile (forReadWrite) the stamp is written to and read from.
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SleepPixelCacheStampTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepPixelCacheStampTest)
