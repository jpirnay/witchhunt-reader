# How FlashArea::get() picks the board's area: one executable per scenario, since the pick is made once per run. The
# real FlashArea.cpp and both backends, over test/flash_lang_partition's in-memory partition, this directory's
# esp_heap_caps.h for the PSRAM, and test/shims' HalStorage for the card (which holds no file).
# scenario;FLASH_AREA_PSRAM_IMAGE[;FLASH_AREA_TEST_PARTITION_KB]. An image of "spiram" leaves FLASH_AREA_PSRAM_IMAGE to
# FlashArea.cpp's default and builds as a board with PSRAM does (CONFIG_SPIRAM=1): the S3 boards.
foreach(case "0;0" "1;1" "2;1" "3;1" "4;2" "5;1" "6;0" "8;0" "9;spiram" "10;spiram" "11;spiram" "12;spiram" "13;0;0"
        "14;0;1024")
  list(GET case 0 scenario)
  list(GET case 1 image)
  list(LENGTH case fields)
  set(target FlashAreaSelectTest${scenario})
  add_executable(${target}
    FlashAreaSelectTest.cpp
    ${REPO_ROOT}/lib/hal/CardImageFile.cpp
    ${REPO_ROOT}/lib/hal/EspPartitionFlash.cpp
    ${REPO_ROOT}/lib/hal/FlashArea.cpp
    ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
    ${REPO_ROOT}/lib/hal/PsramImageFlash.cpp
  )
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REPO_ROOT}/test/flash_lang_partition
    ${REPO_ROOT}/test/shims
    ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/lib/Logging
    ${REPO_ROOT}/lib/Memory
  )
  if(image STREQUAL "spiram")
    target_compile_definitions(${target} PRIVATE FLASH_AREA_SCENARIO=${scenario} CONFIG_SPIRAM=1)
  else()
    target_compile_definitions(${target} PRIVATE FLASH_AREA_SCENARIO=${scenario} FLASH_AREA_PSRAM_IMAGE=${image})
  endif()
  if(fields GREATER 2)
    list(GET case 2 testKb)
    target_compile_definitions(${target} PRIVATE FLASH_AREA_TEST_PARTITION_KB=${testKb})
  endif()
  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
  gtest_discover_tests(${target})
endforeach()

# Every planned restart in src/ syncs the flash area first, or says why it need not (scripts/check_restart_flash_sync.py).
add_test(NAME FlashAreaRestartSync COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/check_restart_flash_sync.py
                                           ${REPO_ROOT})
