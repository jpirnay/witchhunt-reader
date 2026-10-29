add_executable(VerticalTextTest
  VerticalOrientationTest.cpp
  VerticalDrawTest.cpp
  AllocationCounter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerticalOrientation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerticalDraw.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Same include order as test/cjk_layout: the word_size_layout GfxRenderer.h shim must win over lib/GfxRenderer.
target_include_directories(VerticalTextTest PRIVATE
  ${REPO_ROOT}/test/word_size_layout
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(VerticalTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VerticalTextTest)
