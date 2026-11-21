add_executable(ImageScaleTest
  ImageScaleTest.cpp
)

target_include_directories(ImageScaleTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ImageScaleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageScaleTest)
