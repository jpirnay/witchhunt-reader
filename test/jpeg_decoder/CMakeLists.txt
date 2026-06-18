add_executable(JpegDecoderTest
  JpegDecoderTest.cpp
  Stubs.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/TJpgDec/src/tjpgd.c
)

target_include_directories(JpegDecoderTest PRIVATE
  # Our directory first: provides the HalDisplay.h (+ ESP) stub.
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Real stdio-backed FsFile (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: Print.h, HardwareSerial.h, WString.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/JpegToBmpConverter
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/TJpgDec/src
  ${REPO_ROOT}/lib
)

target_compile_definitions(JpegDecoderTest PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(JpegDecoderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JpegDecoderTest)
