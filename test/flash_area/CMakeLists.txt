# The flash area's consumers over a simulated area: SimFlashArea.cpp stands in for lib/hal/FlashArea.cpp, so
# FlashArea::get() is whatever NorFlash a test hands it, glyph_store's SimNorFlash mostly. test/flash_lang_partition
# brings sdkconfig.h (no PSRAM: the boards with a hyphenation region) and the ROM CRC. Logging stays compiled out.
add_executable(FlashAreaTest
  SimFlashArea.cpp
  FlashFontPartitionAreaTest.cpp
  FlashLangPartitionAreaTest.cpp
  FlashAreaStoresTest.cpp
  ${REPO_ROOT}/lib/hal/FlashAreaRegion.cpp
  ${REPO_ROOT}/lib/hal/FlashFontPartition.cpp
  ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationPack.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationStore.cpp
)
target_include_directories(FlashAreaTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Epub
)
target_link_libraries(FlashAreaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(FlashAreaTest)
