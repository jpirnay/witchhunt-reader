# lib/BookProtection: encryption.xml into protect.bin, and protected books read back through ZipFile
# with real AES. OpenSSL stands in for ESP-IDF's AES driver (shims/aes/esp_aes.h) and enciphers the
# test books; without it the suite is skipped.
find_package(OpenSSL)
if(NOT OPENSSL_FOUND)
  message(STATUS "book_protection: OpenSSL not found, BookProtectionTest skipped")
  return()
endif()

add_executable(BookProtectionTest
  BookProtectionTest.cpp
  BookKeyTest.cpp
  ${REPO_ROOT}/lib/BookProtection/BookKey.cpp
  ${REPO_ROOT}/lib/DeviceSecret/DeviceSecret.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/BookProtection/AesCbcCipher.cpp
  ${REPO_ROOT}/lib/BookProtection/EncryptionXmlParser.cpp
  ${REPO_ROOT}/lib/BookProtection/ProtectionManifest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
)

target_include_directories(BookProtectionTest PRIVATE
  # aes/esp_aes.h, wolfssl/wolfcrypt/{aes,hash}.h, nvs.h, esp_random.h: OpenSSL and a map in place
  # of the chip's AES, wolfCrypt's GCM and SHA-256, NVS and the RNG.
  ${CMAKE_CURRENT_SOURCE_DIR}/shims
  ${REPO_ROOT}/test/zip_entry_reader      # real stdio-backed HalStorage.h, TestZip.h
  ${REPO_ROOT}/test/epub_pipeline         # esp_heap_caps.h, whose largest block a test can set
  ${REPO_ROOT}/lib/BookProtection
  ${REPO_ROOT}/lib/DeviceSecret
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims                 # Arduino.h, Print.h, WString.h
)

target_compile_definitions(BookProtectionTest PRIVATE
  FIXTURE_EPUB="${REPO_ROOT}/test/fixtures/moby-dick.epub"
)

target_link_libraries(BookProtectionTest PRIVATE
  crosspoint_test_common
  inflate_reader_host
  SaxParser
  OpenSSL::Crypto
  GTest::gtest_main
)

gtest_discover_tests(BookProtectionTest)
