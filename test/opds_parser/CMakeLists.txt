find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(EXPAT libexpat)
endif()
if(NOT EXPAT_FOUND)
  find_library(EXPAT_LIBRARIES NAMES expat)
  find_path(EXPAT_INCLUDE_DIRS expat.h)
endif()

add_executable(OpdsParserTest
  OpdsParserTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(OpdsParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${EXPAT_INCLUDE_DIRS}
)

target_compile_definitions(OpdsParserTest PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_link_libraries(OpdsParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ${EXPAT_LIBRARIES}
)

gtest_discover_tests(OpdsParserTest)
