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

# A German slot as 2.40 and as 2.41RC left it, and what converting it must yield (legacy_slot_fixture.py).
set(LEGACY_FIXTURES "")
foreach(release 2.40 2.41RC)
  set(slot "${CMAKE_CURRENT_BINARY_DIR}/legacy-${release}-de.slot")
  set(entries "${CMAKE_CURRENT_BINARY_DIR}/legacy-${release}-de.entries")
  add_custom_command(
    OUTPUT ${slot} ${entries}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/legacy_slot_fixture.py ${REPO_ROOT} ${release} ${slot}
            ${entries}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/legacy_slot_fixture.py ${REPO_ROOT}/scripts/gen_i18n.py
            ${REPO_ROOT}/lib/I18n/legacy/${release}.keys ${LANG_PACK_TRANSLATIONS}
    COMMENT "Writing a ${release} language slot for the conversion test"
    VERBATIM
  )
  list(APPEND LEGACY_FIXTURES ${slot} ${entries})
endforeach()
add_custom_target(i18n_legacy_slot DEPENDS ${LEGACY_FIXTURES})

# The generated headers first: a tree that has built firmware has its own (stripped) copies in lib/I18n.
# esp_rom_crc.h and esp_partition.h are the flash_lang_partition test's stand-ins; uzlib is
# font_group_stream's uzlib_font_ring.
set(LANG_PACK_INCLUDES
  ${I18N_GENERATED_DIR}
  ${REPO_ROOT}/test/flash_lang_partition
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/InflateReader
)

# The pack format (lib/I18n/LangPack.cpp) over the generated key tables.
add_executable(LangPackTest
  LangPackTest.cpp
  ${REPO_ROOT}/lib/I18n/LangPack.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
  ${I18N_GENERATED_STRINGS}
)
target_include_directories(LangPackTest PRIVATE ${LANG_PACK_INCLUDES})
target_compile_definitions(LangPackTest PRIVATE LANG_PACKS_DIR="${LANG_PACKS_DIR}")
add_dependencies(LangPackTest i18n_generated i18n_lang_packs)
target_link_libraries(LangPackTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(LangPackTest)

# The conversion of a 2.40 slot, over the real FlashLangPartition on the in-memory partition.
add_executable(LangPackConvertTest
  LangPackConvertTest.cpp
  ${REPO_ROOT}/lib/I18n/LangPack.cpp
  ${REPO_ROOT}/lib/I18n/LangPackConvert.cpp
  ${REPO_ROOT}/lib/hal/EspPartitionFlash.cpp
  ${REPO_ROOT}/lib/hal/FlashArea.cpp
  ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
  ${I18N_GENERATED_STRINGS}
)
target_include_directories(LangPackConvertTest PRIVATE ${LANG_PACK_INCLUDES})
target_compile_definitions(LangPackConvertTest PRIVATE LEGACY_FIXTURES_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(LangPackConvertTest i18n_generated i18n_legacy_slot)
target_link_libraries(LangPackConvertTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(LangPackConvertTest)

# Issue #733: the language slot from a card pack without a usable `spiffs` partition, in the PSRAM image the S3 boards
# take then (FlashArea.cpp as they build it): with none, and with the 80 KB one of the X4 Pro's factory-compatible
# table. No CardImageFile: the image is kept in PSRAM only, and never on the card.
foreach(case "LangSlotPsramImageTest;0" "LangSlotX4ProFactoryTableTest;0x14000")
  list(GET case 0 target)
  list(GET case 1 partitionBytes)
  add_executable(${target}
    LangSlotPsramImageTest.cpp
    ${REPO_ROOT}/lib/I18n/LangPack.cpp
    ${REPO_ROOT}/lib/hal/EspPartitionFlash.cpp
    ${REPO_ROOT}/lib/hal/FlashArea.cpp
    ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
    ${REPO_ROOT}/lib/hal/PsramImageFlash.cpp
    ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
    ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
    ${I18N_GENERATED_STRINGS}
  )
  target_include_directories(${target} PRIVATE
    ${LANG_PACK_INCLUDES}
    ${REPO_ROOT}/test/flash_area_select
    ${REPO_ROOT}/lib/Memory
  )
  target_compile_definitions(${target} PRIVATE LANG_PACKS_DIR="${LANG_PACKS_DIR}" CONFIG_SPIRAM=1
                                               TEST_PARTITION_BYTES=${partitionBytes}u)
  add_dependencies(${target} i18n_generated i18n_lang_packs)
  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    uzlib_font_ring
    GTest::gtest_main
  )
  gtest_discover_tests(${target})
endforeach()

# The generator's side: the pack it writes reads back, and two key names with one hash fail the build.
add_test(NAME GenI18nPacks COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_gen_i18n.py)
