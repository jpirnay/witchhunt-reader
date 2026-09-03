add_executable(TapTargetsTest
  TapTargetsTest.cpp
)

target_include_directories(TapTargetsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(TapTargetsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TapTargetsTest)
