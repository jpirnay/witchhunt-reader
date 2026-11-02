add_executable(OtShaperPortTest
  OtShaperPortTest.cpp
  FixedVectorTest.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(OtShaperPortTest PRIVATE ${OT_SHAPER_INCLUDE_DIR} ${REPO_ROOT}/test/complex_shaper)
target_compile_definitions(OtShaperPortTest PRIVATE SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data")
target_link_libraries(OtShaperPortTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtShaperPortTest)
