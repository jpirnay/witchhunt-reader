add_executable(FrameRegionTest
  FrameRegionTest.cpp
)

target_include_directories(FrameRegionTest PRIVATE
  # FrameRegion.h and TouchTransform.h only; nothing here includes the device GfxRenderer.h.
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(FrameRegionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FrameRegionTest)
