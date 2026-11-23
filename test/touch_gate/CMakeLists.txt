add_executable(TouchGateTest
  TouchGateTest.cpp
)

target_include_directories(TouchGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TouchGateTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(TouchGateTest)
