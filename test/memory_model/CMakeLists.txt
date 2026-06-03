add_executable(MemoryModelTest
  MemoryModelTest.cpp
)

# Pure C++ mocks — no Arduino, no hardware deps
target_link_libraries(MemoryModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MemoryModelTest)
