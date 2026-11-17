add_executable(CarriedSpineNavTest
  CarriedSpineNavTest.cpp
)

target_include_directories(CarriedSpineNavTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CarriedSpineNavTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CarriedSpineNavTest)
