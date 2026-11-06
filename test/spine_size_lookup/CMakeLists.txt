add_executable(SpineSizeLookupTest
  SpineSizeLookupTest.cpp
)

target_include_directories(SpineSizeLookupTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(SpineSizeLookupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SpineSizeLookupTest)
