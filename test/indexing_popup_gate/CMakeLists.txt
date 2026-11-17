add_executable(IndexingPopupGateTest
  IndexingPopupGateTest.cpp
)

target_include_directories(IndexingPopupGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(IndexingPopupGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IndexingPopupGateTest)
