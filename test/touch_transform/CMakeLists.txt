add_executable(TouchTransformTest
  TouchTransformTest.cpp
)

target_include_directories(TouchTransformTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(TouchTransformTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchTransformTest)
