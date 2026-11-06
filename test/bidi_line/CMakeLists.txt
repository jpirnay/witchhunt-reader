add_executable(BidiLineTest
  BidiLineTest.cpp
  ${BIDI_LAYOUT_SOURCES}
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BidiLineTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h (real one needs Arduino)
  ${BIDI_LAYOUT_INCLUDES}
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/shims  # Arduino.h: the shaped store's heap check (ESP.getMaxAllocHeap)
)

target_link_libraries(BidiLineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BidiLineTest)
