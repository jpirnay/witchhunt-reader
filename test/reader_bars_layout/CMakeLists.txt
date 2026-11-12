# The reader bars' layout (src/activities/reader/ReaderBarsLayout.h) at every board's size, both orientations. FreeInkUI
# alone: the layout header needs nothing else of the firmware's.
add_executable(ReaderBarsLayoutTest
  ReaderBarsLayoutTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(ReaderBarsLayoutTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

target_link_libraries(ReaderBarsLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderBarsLayoutTest)
