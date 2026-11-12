add_executable(ReaderLayoutControlsTest
  ReaderLayoutControlsTest.cpp
)

target_include_directories(ReaderLayoutControlsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  # The stdio-backed HalStorage CrossPointSettings.h pulls in.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(ReaderLayoutControlsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderLayoutControlsTest)
