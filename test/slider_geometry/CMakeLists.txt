add_executable(SliderGeometryTest
  SliderGeometryTest.cpp
)

target_include_directories(SliderGeometryTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SliderGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SliderGeometryTest)
