# lib/hal/PanicReboot.h, with this directory's esp_system.h standing in for ESP-IDF's.
add_executable(PanicRebootTest
  PanicRebootTest.cpp
)

target_include_directories(PanicRebootTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(PanicRebootTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PanicRebootTest)
