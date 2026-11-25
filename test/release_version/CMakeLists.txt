# Release version parsing and ordering (src/network/ReleaseVersion.h), against the sscanf format it replaced.
add_executable(ReleaseVersionTest
  ReleaseVersionTest.cpp
  ${REPO_ROOT}/src/network/ReleaseVersion.cpp
)

target_include_directories(ReleaseVersionTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(ReleaseVersionTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ReleaseVersionTest)
