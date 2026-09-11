add_executable(ListScrollBarTest
  ListScrollBarTest.cpp
)

target_include_directories(ListScrollBarTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ListScrollBarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListScrollBarTest)
