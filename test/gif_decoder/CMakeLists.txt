add_executable(GifDecoderTest
  GifDecoderTest.cpp
  Stubs.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(GifDecoderTest PRIVATE
  # Our directory must be first: provides GfxRenderer.h, HalDisplay.h,
  # esp_task_wdt.h stubs that shadow the real device headers.
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Real stdio-backed FsFile (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: HardwareSerial.h, WString.h, Print.h, Arduino.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Epub/Epub/converters
  # AdaptiveTone.h, pulled in by RenderConfig. Listed after this directory so the
  # local GfxRenderer.h stub still shadows the real one alongside it.
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib
)

target_link_libraries(GifDecoderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GifDecoderTest)
