# Decimal fixed point as text and from a float (lib/Serialization/FixedDecimal.h), against the printf, lround and strtod
# results it replaced.
add_executable(FixedDecimalTest
  FixedDecimalTest.cpp
  ${REPO_ROOT}/lib/Serialization/FixedDecimal.cpp
)

target_include_directories(FixedDecimalTest PRIVATE
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(FixedDecimalTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(FixedDecimalTest)
