add_executable(PasswordMaskTest
  PasswordMaskTest.cpp
)

target_include_directories(PasswordMaskTest PRIVATE
  ${REPO_ROOT}/src/activities/util
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(PasswordMaskTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(PasswordMaskTest)
