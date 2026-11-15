# The hyphenation pack path: the CPHY v1 header, the flash store over a simulated chip, the registry with every
# language but English served from the store, and the trie read in place from its pack file (PagedTrieReader). Without HYPHENATION_ALL_BUILTIN, as the firmware builds it.
get_directory_property(HYPH_STORE_DEFS COMPILE_DEFINITIONS)
list(REMOVE_ITEM HYPH_STORE_DEFS HYPHENATION_ALL_BUILTIN)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${HYPH_STORE_DEFS}")

# The packs the repo hosts (assets/sd-fonts/Hyphenation-*/, scripts/build_hyphenation_packs.py): the test checks they
# hold the built-in tries, and that the languages added with packs break as their patterns do.
set(HYPH_PACK_DIR "${REPO_ROOT}/assets/sd-fonts")

add_executable(HyphenationStoreTest
  HyphenationStoreTest.cpp
  PagedTrieReaderTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationPack.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
# esp_rom_crc.h: flash_lang_partition's stand-in for the ROM CRC. SimNorFlash: glyph_store's simulated chip.
target_include_directories(HyphenationStoreTest PRIVATE
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)
target_compile_definitions(HyphenationStoreTest PRIVATE
  HYPHENATION_RESOURCES_DIR="${REPO_ROOT}/test/hyphenation_eval/resources"
  HYPHENATION_PACK_DIR="${HYPH_PACK_DIR}"
  HYPHENATION_PACK_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)
target_link_libraries(HyphenationStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(HyphenationStoreTest)
