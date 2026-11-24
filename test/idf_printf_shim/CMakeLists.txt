# The IDF printf shim (src/platform/IdfPrintfShim.h): vsnprintf in place of newlib's stream formatter.
add_executable(IdfPrintfShimTest
  IdfPrintfShimTest.cpp
  ${REPO_ROOT}/src/platform/IdfPrintfShim.cpp
)

target_include_directories(IdfPrintfShimTest PRIVATE
  ${REPO_ROOT}/src/platform
)

target_link_libraries(IdfPrintfShimTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(IdfPrintfShimTest)
