# Plugin events without the card or the network (src/util/PluginEventCore.h) and the reading-session summary
# (src/activities/reader/ReaderSession.h). ArduinoJson is the target kosync_progress_reply fetches. The token and
# config files resolve through src/util/PluginLocations.h (FsHelpers, the String shim), and the event URLs are judged
# by src/network/PluginHosts.h (lib/SecureNet's UrlTarget).
add_executable(PluginEventsTest
  PluginEventCoreTest.cpp
  ReaderSessionTest.cpp
  ${REPO_ROOT}/src/util/PluginEventCore.cpp
  ${REPO_ROOT}/lib/Serialization/JsonText.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderSession.cpp
  ${REPO_ROOT}/src/network/PluginHosts.cpp
  ${REPO_ROOT}/lib/SecureNet/src/UrlTarget.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_include_directories(PluginEventsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/SecureNet/include
  ${REPO_ROOT}/test/shims
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
