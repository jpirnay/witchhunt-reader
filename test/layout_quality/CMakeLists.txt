add_executable(LayoutQualityTest
  LayoutQualityTest.cpp
)

# Pure C++ logic — no Arduino, no hardware deps
target_link_libraries(LayoutQualityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LayoutQualityTest)
