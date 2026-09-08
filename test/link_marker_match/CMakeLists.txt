add_executable(LinkMarkerMatchTest
  LinkMarkerMatchTest.cpp
)

target_include_directories(LinkMarkerMatchTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LinkMarkerMatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LinkMarkerMatchTest)
