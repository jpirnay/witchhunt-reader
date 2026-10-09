add_executable(SdFontKernTest
  SdFontKernTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(SdFontKernTest PRIVATE
  ${REPO_ROOT}/test/zip_entry_reader  # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SdFontKernTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SdFontKernTest PROPERTIES TIMEOUT 60)
