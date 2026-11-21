add_executable(ButtonActionMirrorTest
  ButtonActionMirrorTest.cpp
)

target_include_directories(ButtonActionMirrorTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(ButtonActionMirrorTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(ButtonActionMirrorTest)
