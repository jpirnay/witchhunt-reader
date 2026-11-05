add_executable(MultipartRouteTest
  MultipartRouteTest.cpp
)

target_include_directories(MultipartRouteTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)

target_link_libraries(MultipartRouteTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(MultipartRouteTest)
