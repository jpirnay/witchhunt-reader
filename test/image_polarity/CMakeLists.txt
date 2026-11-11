add_executable(ImagePolarityTest
  ImagePolarityTest.cpp
)

target_include_directories(ImagePolarityTest PRIVATE
  # ImagePolarity.h and the TouchTransform.h it takes the orientation order from.
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ImagePolarityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImagePolarityTest)
