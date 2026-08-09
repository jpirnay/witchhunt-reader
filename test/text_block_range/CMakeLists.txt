add_executable(TextBlockRangeTest
  TextBlockRangeTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Include order matters: this directory's GfxRenderer.h shim must win over the
# real lib/GfxRenderer (which drags in HalDisplay); lib/GfxRenderer is therefore
# deliberately absent from the include path.
target_include_directories(TextBlockRangeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(TextBlockRangeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextBlockRangeTest)
