add_executable(CacheCipherTest
  CacheCipherTest.cpp
  CacheKeysTest.cpp
  ${REPO_ROOT}/lib/CacheCipher/CacheCipher.cpp
  ${REPO_ROOT}/lib/CacheCipher/CacheKeys.cpp
)

target_include_directories(CacheCipherTest PRIVATE
  ${REPO_ROOT}/lib/CacheCipher
)

target_link_libraries(CacheCipherTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(CacheCipherTest)
