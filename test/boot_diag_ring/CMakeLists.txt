add_executable(BootDiagRingTest
  BootDiagRingTest.cpp
)

# Pure C++ — the header under test deliberately depends on nothing but <cstdint>/<cstring>,
# so no Arduino shim, no HAL, no SD card.
target_include_directories(BootDiagRingTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(BootDiagRingTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(BootDiagRingTest)
