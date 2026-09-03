add_executable(FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)

# The board name is a compile-time choice; build the tests as the lilygo image
# so a foreign tag has something to disagree with.
target_compile_definitions(FirmwareBoardTagTest PRIVATE FREEINK_DEVICE_LILYGO=1)

target_include_directories(FirmwareBoardTagTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/src/network
)

target_link_libraries(FirmwareBoardTagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareBoardTagTest)
