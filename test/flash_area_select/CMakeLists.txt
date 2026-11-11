# How FlashArea::get() picks the board's area: one executable per scenario, since the pick is made once per run. The
# real FlashArea.cpp and both backends, over test/flash_lang_partition's in-memory partition and this directory's
# esp_heap_caps.h for the PSRAM. CardImageFile.cpp is linked only where the image is kept on the card
# (FLASH_AREA_PSRAM_ON_CARD=1): every other scenario builds without it, which shows the default image never writes to
# the card; test/shims' HalStorage stands in for the card (which holds no file).
# scenario;FLASH_AREA_PSRAM_IMAGE;FLASH_AREA_PSRAM_ON_CARD. An image of "spiram" leaves FLASH_AREA_PSRAM_IMAGE to
# FlashArea.cpp's default and builds as a board with PSRAM does (CONFIG_SPIRAM=1): the S3 boards.
foreach(case "0;0;0" "1;1;0" "2;1;0" "3;1;0" "4;2;0" "5;1;0" "6;0;0" "7;1;1" "8;0;0" "9;spiram;0" "10;spiram;0")
  list(GET case 0 scenario)
  list(GET case 1 image)
  list(GET case 2 onCard)
  set(target FlashAreaSelectTest${scenario})
  set(sources
    FlashAreaSelectTest.cpp
    ${REPO_ROOT}/lib/hal/EspPartitionFlash.cpp
    ${REPO_ROOT}/lib/hal/FlashArea.cpp
    ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
    ${REPO_ROOT}/lib/hal/PsramImageFlash.cpp
  )
  if(onCard)
    list(APPEND sources ${REPO_ROOT}/lib/hal/CardImageFile.cpp)
  endif()
  add_executable(${target} ${sources})
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
    target_compile_definitions(${target} PRIVATE FLASH_AREA_SCENARIO=${scenario} FLASH_AREA_PSRAM_IMAGE=${image}
                                                 FLASH_AREA_PSRAM_ON_CARD=${onCard})
  endif()
  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
  gtest_discover_tests(${target})
endforeach()
