add_executable(CarouselCoverLayoutTest
  CarouselCoverLayoutTest.cpp
)

target_include_directories(CarouselCoverLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CarouselCoverLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CarouselCoverLayoutTest)
