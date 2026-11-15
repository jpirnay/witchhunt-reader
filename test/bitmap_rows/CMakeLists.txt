add_executable(BitmapRowsTest
  BitmapRowsTest.cpp
  BitmapRowTargetTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/test/jpeg_decoder/Stubs.cpp
)

target_include_directories(BitmapRowsTest PRIVATE
  # The HalDisplay.h (+ ESP) stub BitmapHelpers reaches for.
  ${REPO_ROOT}/test/jpeg_decoder
  # Real stdio-backed FsFile (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: Print.h, HardwareSerial.h, WString.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib
)

target_link_libraries(BitmapRowsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BitmapRowsTest)
