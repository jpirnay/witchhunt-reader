add_library(SaxParser STATIC
  SaxParserYxml.cpp
  yxml.c
)

# yxml.c is plain C; force it to compile as C++ so the parent project
# only needs to enable CXX (the test suite declares project(... CXX)).
set_source_files_properties(yxml.c PROPERTIES LANGUAGE CXX)

target_include_directories(SaxParser PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
