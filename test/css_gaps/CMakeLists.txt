add_executable(CssGapsTest
  CssGapsTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(CssGapsTest PRIVATE
  ${REPO_ROOT}/test/shims       # HalStorage.h, HardwareSerial.h, Print.h, WString.h
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
)

target_compile_definitions(CssGapsTest PRIVATE
  # No ENABLE_SERIAL_LOG → all LOG_* macros are no-ops; logPrintf never called
  # No Arduino USB flags needed — logSerial path is dead
)

# Provide stub definitions for the few non-inline symbols Logging.h exposes
# (logPrintf, getLastLogs, clearLastLogs, sanitizeLogHead, MySerialImpl).
# Rather than compiling Logging.cpp (which uses RTC_NOINIT_ATTR / millis()),
# we define minimal stubs inline here.
target_sources(CssGapsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/LoggingStub.cpp
)

target_link_libraries(CssGapsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssGapsTest)
