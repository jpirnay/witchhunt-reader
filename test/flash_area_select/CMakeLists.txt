# How FlashArea::get() picks the board's area: one executable per scenario, since the pick is made once per run. The
# real FlashArea.cpp and both backends, over test/flash_lang_partition's in-memory partition, this directory's
# esp_heap_caps.h for the PSRAM, and test/shims' HalStorage for the card (which holds no file).
# scenario;FLASH_AREA_PSRAM_IMAGE
foreach(case "0;0" "1;1" "2;1" "3;1" "4;2")
  list(GET case 0 scenario)
  list(GET case 1 image)
  set(target FlashAreaSelectTest${scenario})
  add_executable(${target}
    FlashAreaSelectTest.cpp
    ${REPO_ROOT}/lib/hal/CardImageFile.cpp
    ${REPO_ROOT}/lib/hal/EspPartitionFlash.cpp
    ${REPO_ROOT}/lib/hal/FlashArea.cpp
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
  target_compile_definitions(${target} PRIVATE FLASH_AREA_SCENARIO=${scenario} FLASH_AREA_PSRAM_IMAGE=${image})
  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
  gtest_discover_tests(${target})
endforeach()
