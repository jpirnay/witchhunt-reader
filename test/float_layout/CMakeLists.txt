add_executable(FloatLayoutTest
  FloatLayoutTest.cpp
)

# Pure C++ logic — no Arduino, no hardware deps
target_link_libraries(FloatLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FloatLayoutTest)
