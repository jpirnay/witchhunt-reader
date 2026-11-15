# JsonFile (lib/Serialization) and a store that writes through it, over an in-memory card (stubs/) and
# the String shim's growLimit(). ArduinoJson is the target kosync_progress_reply fetches; its String
# support, off on a host by default, is what the firmware serialises through.
add_executable(JsonFileTest
  JsonFileTest.cpp
  ${REPO_ROOT}/lib/Serialization/JsonFile.cpp
  ${REPO_ROOT}/lib/Weather/WeatherSettingsStore.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(JsonFileTest PRIVATE
  # The in-memory HalStorage.h; shadows the test/shims stub.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Weather
)

target_compile_definitions(JsonFileTest PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

target_link_libraries(JsonFileTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(JsonFileTest)
