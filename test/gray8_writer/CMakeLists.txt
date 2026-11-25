add_executable(Gray8WriterTest Gray8WriterTest.cpp DirectPixelWriterPolarityTest.cpp DirectPixelWriterRowsTest.cpp)

target_include_directories(Gray8WriterTest PRIVATE
  # Stub GfxRenderer.h / HalDisplay.h must shadow the real device headers.
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Epub/Epub/converters
  # PackedPlaneTiles.h, after the stubs so their GfxRenderer.h still wins.
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(Gray8WriterTest PRIVATE GTest::gtest_main)

gtest_discover_tests(Gray8WriterTest)
