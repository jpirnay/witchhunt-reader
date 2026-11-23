# The weather's integer units (lib/Weather/WeatherUnits.h): what the screens show, pinned against the float code they
# replaced, from the API's JSON through ArduinoJson; and the settings file, read and written both ways. The in-memory
# card is json_file's.
add_executable(WeatherUnitsTest
  WeatherUnitsTest.cpp
  ${REPO_ROOT}/lib/Serialization/FixedDecimal.cpp
  ${REPO_ROOT}/lib/Serialization/JsonFile.cpp
  ${REPO_ROOT}/lib/Weather/WeatherSettingsStore.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(WeatherUnitsTest PRIVATE
  # The in-memory HalStorage.h; shadows the test/shims stub.
  ${REPO_ROOT}/test/json_file/stubs
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Weather
)

target_compile_definitions(WeatherUnitsTest PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

target_link_libraries(WeatherUnitsTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(WeatherUnitsTest)
