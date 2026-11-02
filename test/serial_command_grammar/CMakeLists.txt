add_executable(SerialCommandGrammarTest
  SerialCommandGrammarTest.cpp
  ${REPO_ROOT}/src/util/SerialCommandGrammar.cpp
  # The CRC32 the file commands check (CMD:FS:PUT, GET, HASH) is the transfer protocol's.
  ${REPO_ROOT}/lib/SerialTransfer/SerialTransfer/SerialTransferProtocol.cpp
)

target_include_directories(SerialCommandGrammarTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/SerialTransfer
)

target_link_libraries(SerialCommandGrammarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerialCommandGrammarTest)
