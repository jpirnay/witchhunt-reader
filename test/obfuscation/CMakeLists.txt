add_executable(ObfuscationUtilsTest
  ObfuscationUtilsTest.cpp
  ${REPO_ROOT}/lib/Serialization/ObfuscationUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ObfuscationUtilsTest PRIVATE
  # esp_mac.h (a fixed reader MAC) and base64.h, the two Arduino/ESP-IDF headers it reads.
  ${CMAKE_CURRENT_SOURCE_DIR}/shims
  # Arduino.h / WString.h, and wolfssl/wolfcrypt/coding.h for Base64_Decode.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(ObfuscationUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ObfuscationUtilsTest)
