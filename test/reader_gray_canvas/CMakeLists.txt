add_executable(ReaderGrayCanvasTest
  ReaderGrayCanvasTest.cpp
)

target_include_directories(ReaderGrayCanvasTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  # Gray8Compose.h only; nothing here includes the device GfxRenderer.h.
  ${REPO_ROOT}/lib/GfxRenderer
  # Epub/blocks/PictureTone.h
  ${REPO_ROOT}/lib/Epub
  # The stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(ReaderGrayCanvasTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderGrayCanvasTest)
