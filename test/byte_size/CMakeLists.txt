# Byte counts as text (src/util/ByteSize.h), against the printf("%.1f") of a double they replaced.
add_executable(ByteSizeTest
  ByteSizeTest.cpp
  ${REPO_ROOT}/src/util/ByteSize.cpp
)

target_include_directories(ByteSizeTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ByteSizeTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ByteSizeTest)
