# The hyphenation pack path: the CPHY v1 header, the flash store over a simulated chip, and the registry with every
# language but English served from the store. Without HYPHENATION_ALL_BUILTIN, as the firmware builds it.
get_directory_property(HYPH_STORE_DEFS COMPILE_DEFINITIONS)
list(REMOVE_ITEM HYPH_STORE_DEFS HYPHENATION_ALL_BUILTIN)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${HYPH_STORE_DEFS}")

# The packs as the release builds them, from the generated headers: the test checks they hold the built-in tries.
set(HYPH_PACK_DIR "${CMAKE_CURRENT_BINARY_DIR}/packs")
file(GLOB HYPH_TRIE_HEADERS "${REPO_ROOT}/lib/Epub/Epub/hyphenation/generated/*.trie.h")
add_custom_command(
  OUTPUT ${HYPH_PACK_DIR}/hyph-de.cphyph
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/build_hyphenation_packs.py --out ${HYPH_PACK_DIR}
  DEPENDS ${REPO_ROOT}/scripts/build_hyphenation_packs.py ${HYPH_TRIE_HEADERS}
  COMMENT "Building hyphenation packs for host tests"
  VERBATIM
)
add_custom_target(hyphenation_packs DEPENDS ${HYPH_PACK_DIR}/hyph-de.cphyph)

add_executable(HyphenationStoreTest
  HyphenationStoreTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationPack.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
add_dependencies(HyphenationStoreTest hyphenation_packs)
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
)
target_link_libraries(HyphenationStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(HyphenationStoreTest)
