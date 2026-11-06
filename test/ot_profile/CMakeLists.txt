# The shaper profile (throwaway): the same test twice, once with the instrumentation (OT_PROFILE) and once without, to
# measure its overhead. Environment: OT_PROFILE_FONT (a .ttf), OT_PROFILE_CHAPTER (an .xhtml).
add_executable(ShaperProfileTest ShaperProfileTest.cpp ${OT_SHAPER_SOURCES})
target_include_directories(ShaperProfileTest PRIVATE ${OT_SHAPER_INCLUDE_DIR})
target_compile_definitions(ShaperProfileTest PRIVATE OT_PROFILE)
target_link_libraries(ShaperProfileTest PRIVATE crosspoint_test_common GTest::gtest_main)

add_executable(ShaperProfileBaseTest ShaperProfileTest.cpp ${OT_SHAPER_SOURCES})
target_include_directories(ShaperProfileBaseTest PRIVATE ${OT_SHAPER_INCLUDE_DIR})
target_link_libraries(ShaperProfileBaseTest PRIVATE crosspoint_test_common GTest::gtest_main)
