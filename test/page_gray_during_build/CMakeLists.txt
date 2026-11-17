add_executable(BuildPageGrayTest
  BuildPageGrayTest.cpp
  RedrawFrameTest.cpp
)

target_include_directories(BuildPageGrayTest PRIVATE
  ${REPO_ROOT}/src
  # FrameRegion.h and TouchTransform.h only; nothing here includes the device GfxRenderer.h.
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(BuildPageGrayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildPageGrayTest)
