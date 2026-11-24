# Plugin events without the card or the network (src/util/PluginEventCore.h) and the reading-session summary
# (src/activities/reader/ReaderSession.h). ArduinoJson is the target kosync_progress_reply fetches.
add_executable(PluginEventsTest
  PluginEventCoreTest.cpp
  ReaderSessionTest.cpp
  ${REPO_ROOT}/src/util/PluginEventCore.cpp
  ${REPO_ROOT}/lib/Serialization/JsonText.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderSession.cpp
)

target_include_directories(PluginEventsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PluginEventsTest PRIVATE
  ArduinoJson
  GTest::gtest_main
)

# The plugins the web Home Screen page makes; test/web/home_layout_test.mjs checks the page makes these.
target_compile_definitions(PluginEventsTest PRIVATE
  HOME_PICTURE_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/home_picture_plugins.json"
)

gtest_discover_tests(PluginEventsTest)
