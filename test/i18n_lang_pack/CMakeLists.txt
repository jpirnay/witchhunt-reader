enable_language(C)

# See timezone/CMakeLists.txt: the generated string blob has to be marked GENERATED again in every
# subdirectory that names it, or a clean clone fails to configure.
set_source_files_properties(${I18N_GENERATED_STRINGS} PROPERTIES GENERATED TRUE)

# The packs the generator writes for the card, into the build tree: what the test reads as the device
# would.
set(LANG_PACKS_DIR "${CMAKE_CURRENT_BINARY_DIR}/packs")
file(GLOB LANG_PACK_TRANSLATIONS "${REPO_ROOT}/lib/I18n/translations/*.yaml")
add_custom_command(
  OUTPUT ${LANG_PACKS_DIR}/Language-German/de.cplang
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations
          --packs ${LANG_PACKS_DIR}
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${LANG_PACK_TRANSLATIONS}
  COMMENT "Writing the language packs for the host tests"
  VERBATIM
)
add_custom_target(i18n_lang_packs DEPENDS ${LANG_PACKS_DIR}/Language-German/de.cplang)

# The pack format (lib/I18n/LangPack.cpp) over the generated key tables. esp_rom_crc.h is the
# flash_lang_partition test's stand-in for the ROM CRC; uzlib is font_group_stream's uzlib_font_ring.
add_executable(LangPackTest
  LangPackTest.cpp
  ${REPO_ROOT}/lib/I18n/LangPack.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
  ${I18N_GENERATED_STRINGS}
)
# The generated headers first: a tree that has built firmware has its own (stripped) copies in lib/I18n.
target_include_directories(LangPackTest PRIVATE
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib/InflateReader
)
target_compile_definitions(LangPackTest PRIVATE LANG_PACKS_DIR="${LANG_PACKS_DIR}")
add_dependencies(LangPackTest i18n_generated i18n_lang_packs)
target_link_libraries(LangPackTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(LangPackTest)

# The generator's side: the pack it writes reads back, and two key names with one hash fail the build.
add_test(NAME GenI18nPacks COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_gen_i18n.py)
