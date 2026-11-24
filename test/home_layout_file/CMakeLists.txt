# Lyra Tiles' layout file (src/components/themes/lyra/HomeLayoutFile). ArduinoJson is the target
# kosync_progress_reply fetches, so this is added after it.
add_executable(HomeLayoutFileTest
  HomeLayoutFileTest.cpp
  ${REPO_ROOT}/src/components/themes/lyra/HomeLayoutFile.cpp
  ${REPO_ROOT}/lib/Serialization/JsonText.cpp
)

target_include_directories(HomeLayoutFileTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(HomeLayoutFileTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(HomeLayoutFileTest)
