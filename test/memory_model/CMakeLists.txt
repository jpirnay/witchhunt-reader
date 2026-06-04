add_executable(MemoryModelTest
  MemoryModelTest.cpp
)

# Pure C++ mocks — no Arduino, no hardware deps
target_include_directories(MemoryModelTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/hal
)

target_link_libraries(MemoryModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MemoryModelTest)
