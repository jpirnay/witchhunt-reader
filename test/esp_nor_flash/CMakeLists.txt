# The real EspNorFlash and FlashFontPartition over test/flash_lang_partition's in-memory `spiffs` partition: which
# mappings keep the .cpfont cache off the partition. This directory's sdkconfig.h (no PSRAM: the boards with a
# hyphenation region) and spi_flash_mmap.h stand in for ESP-IDF's. Logging stays compiled out.
add_executable(EspNorFlashTest
  EspNorFlashTest.cpp
  ${REPO_ROOT}/lib/hal/EspNorFlash.cpp
  ${REPO_ROOT}/lib/hal/FlashFontPartition.cpp
)
target_include_directories(EspNorFlashTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
)
target_link_libraries(EspNorFlashTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(EspNorFlashTest)
