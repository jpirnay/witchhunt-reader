add_executable(ProgressPpbTest
  ProgressPpbTest.cpp
)

target_include_directories(ProgressPpbTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  # util/PrintfDouble.h
  ${REPO_ROOT}/src
)

target_link_libraries(ProgressPpbTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressPpbTest)
