add_executable(BayerDitherTest BayerDitherTest.cpp)

# DitherUtils.h is header-only.
target_include_directories(BayerDitherTest PRIVATE ${REPO_ROOT}/lib/Epub/Epub/converters)

target_link_libraries(BayerDitherTest PRIVATE GTest::gtest_main)

gtest_discover_tests(BayerDitherTest)
