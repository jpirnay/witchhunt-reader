add_executable(HomeTilesLayoutTest
  HomeTilesLayoutTest.cpp
)

target_include_directories(HomeTilesLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeTilesLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeTilesLayoutTest)
