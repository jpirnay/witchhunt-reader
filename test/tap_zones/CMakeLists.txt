add_executable(TapZonesTest
  TapZonesTest.cpp
)

target_include_directories(TapZonesTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TapZonesTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(TapZonesTest)
