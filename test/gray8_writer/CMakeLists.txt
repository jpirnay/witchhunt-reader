add_executable(Gray8WriterTest Gray8WriterTest.cpp)

target_include_directories(Gray8WriterTest PRIVATE
  # Stub GfxRenderer.h / HalDisplay.h must shadow the real device headers.
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Epub/Epub/converters
)

target_link_libraries(Gray8WriterTest PRIVATE GTest::gtest_main)

gtest_discover_tests(Gray8WriterTest)
