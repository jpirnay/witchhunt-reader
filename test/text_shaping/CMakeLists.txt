add_executable(TextShapingTest
  TextShapingTest.cpp
  ${REPO_ROOT}/lib/TextShaping/TextShaping.cpp
  ${REPO_ROOT}/lib/TextShaping/HebrewShaper.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TextShapingTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../minibidi_arabic/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/TextShaping
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TextShapingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextShapingTest)
