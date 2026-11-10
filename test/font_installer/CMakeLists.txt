# FontInstaller over an in-memory card: whether a family on the card has an update (a pack's files compared by their
# CRC too, a font's by their size), and the CRC of a file as it reads it.
add_executable(FontInstallerTest
  FontInstallerTest.cpp
  ${REPO_ROOT}/src/FontInstaller.cpp
  ${REPO_ROOT}/src/util/FontCatalog.cpp
)

# The real I18n.h, for I18n::PACK_DIR: it includes the generated I18nKeys.h (see test/CMakeLists.txt).
add_dependencies(FontInstallerTest i18n_generated)

target_include_directories(FontInstallerTest PRIVATE
  # The in-memory card with FAT's case-blind names, ahead of test/shims' HalStorage.h, whose exists() is always false.
  ${REPO_ROOT}/test/file_replace
  # esp_rom_crc.h: the ROM CRC as zlib computes it.
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/I18n
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(FontInstallerTest PRIVATE
  SD_FONTS_DIR="${REPO_ROOT}/assets/sd-fonts"
)

target_link_libraries(FontInstallerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontInstallerTest)
