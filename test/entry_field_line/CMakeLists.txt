add_executable(EntryFieldLineTest
  EntryFieldLineTest.cpp
  ${REPO_ROOT}/src/activities/util/EntryFieldLine.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(EntryFieldLineTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../minibidi_arabic/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/src/activities/util
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(EntryFieldLineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EntryFieldLineTest)
