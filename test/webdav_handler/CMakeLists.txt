add_executable(WebDAVHandlerTest
  WebDAVHandlerTest.cpp
  BookCacheResetTest.cpp
  WebPathGuardTest.cpp
  stubs/BookRelocation.cpp
  stubs/BookSession.cpp
  ${REPO_ROOT}/src/network/WebDAVHandler.cpp
  ${REPO_ROOT}/src/network/WebPathGuard.cpp
  ${REPO_ROOT}/src/BookCacheReset.cpp
  ${REPO_ROOT}/lib/FsHelpers/BookCache.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_include_directories(WebDAVHandlerTest PRIVATE
  # The framework's WebServer, String and NetworkClient and an in-memory card; they shadow the test/shims stubs of
  # the same names.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/lib/BookProtection
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

# UploadStallTolerance.h takes its limit from platformio.ini's build flags.
target_compile_definitions(WebDAVHandlerTest PRIVATE WEBSOCKETS_TCP_TIMEOUT=10000)

target_link_libraries(WebDAVHandlerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WebDAVHandlerTest)
