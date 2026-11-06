add_executable(OtShaperPortTest
  OtShaperPortTest.cpp
  FixedVectorTest.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(OtShaperPortTest PRIVATE ${OT_SHAPER_INCLUDE_DIR} ${REPO_ROOT}/test/complex_shaper
                           ${REPO_ROOT}/test/ttf_font)
target_compile_definitions(OtShaperPortTest PRIVATE SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data"
                                                   TTF_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/fonts")
target_link_libraries(OtShaperPortTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtShaperPortTest)

# An executable of its own: it replaces the global operator new and delete.
add_executable(OtShaperAllocTest
  OtShaperAllocTest.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(OtShaperAllocTest PRIVATE ${OT_SHAPER_INCLUDE_DIR} ${REPO_ROOT}/test/complex_shaper)
target_compile_definitions(OtShaperAllocTest PRIVATE SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data")
target_link_libraries(OtShaperAllocTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtShaperAllocTest)

# The measure-first probe: a host tool, not a test, so built on request. It replaces the global operator new and delete.
add_executable(ot_shaper_probe EXCLUDE_FROM_ALL OtShaperProbe.cpp ${OT_SHAPER_SOURCES})
target_include_directories(ot_shaper_probe PRIVATE ${OT_SHAPER_INCLUDE_DIR})
target_link_libraries(ot_shaper_probe PRIVATE crosspoint_test_common)
