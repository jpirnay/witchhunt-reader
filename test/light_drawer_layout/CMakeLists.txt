# The light drawer's layout (src/activities/util/LightDrawerLayout.h) at every lit board's size. FreeInkUI alone:
# the layout header needs nothing else of the firmware's.
add_executable(LightDrawerLayoutTest
  LightDrawerLayoutTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(LightDrawerLayoutTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

target_link_libraries(LightDrawerLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LightDrawerLayoutTest)
