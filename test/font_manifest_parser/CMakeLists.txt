add_executable(FontManifestParserTest
  FontManifestParserTest.cpp
  FontManifestReaderTest.cpp
  FontCatalogTest.cpp
  FontDownloadRulesTest.cpp
  # Replaces the global nothrow array new, so the tests can fail the catalog's block allocation.
  FailingArrayNew.cpp
  ${REPO_ROOT}/src/util/FontCatalog.cpp
  ${REPO_ROOT}/src/util/FontManifestReader.cpp
  ${REPO_ROOT}/lib/JsonParser/FontManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(FontManifestParserTest PRIVATE
  # HalStorage.h whose HalFile reads from a string; the real one in lib/hal wants SdFat.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(FontManifestParserTest PRIVATE
  FONT_MANIFEST_PATH="${REPO_ROOT}/assets/sd-fonts/fonts.json"
  FONT_MANIFEST_V3_PATH="${REPO_ROOT}/assets/sd-fonts/fonts-v3.json"
  SD_FONTS_DIR="${REPO_ROOT}/assets/sd-fonts"
)

target_link_libraries(FontManifestParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontManifestParserTest)
