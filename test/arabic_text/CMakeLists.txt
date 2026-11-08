add_executable(ArabicScriptsTest
  ArabicScriptsTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ArabicScriptsTest PRIVATE
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(ArabicScriptsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ArabicScriptsTest)
