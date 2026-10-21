add_executable(TlsPolicyTest
  TlsPolicyTest.cpp
  ${REPO_ROOT}/lib/SecureNet/src/TlsPolicy.cpp
)

target_include_directories(TlsPolicyTest PRIVATE
  ${REPO_ROOT}/lib/SecureNet/include
)

target_link_libraries(TlsPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TlsPolicyTest)
