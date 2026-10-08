add_executable(EpubProgressRecordTest
  EpubProgressRecordTest.cpp
  EpubLinkBackStackTest.cpp
)

target_include_directories(EpubProgressRecordTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(EpubProgressRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubProgressRecordTest)
