# When the emergency-mode notice is due (src/FlashLayoutNotice.h): its pure part, which needs no board.
add_executable(FlashLayoutNoticeTest FlashLayoutNoticeTest.cpp)
target_include_directories(FlashLayoutNoticeTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(FlashLayoutNoticeTest PRIVATE GTest::gtest_main)
gtest_discover_tests(FlashLayoutNoticeTest)
