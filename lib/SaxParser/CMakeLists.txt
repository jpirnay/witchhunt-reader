find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(EXPAT libexpat)
endif()
if(NOT EXPAT_FOUND)
  find_library(EXPAT_LIBRARIES NAMES expat)
  find_path(EXPAT_INCLUDE_DIRS expat.h)
endif()

add_library(SaxParser STATIC
  SaxParserExpat.cpp
)

target_include_directories(SaxParser PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${EXPAT_INCLUDE_DIRS}
)

target_compile_definitions(SaxParser PUBLIC
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_link_libraries(SaxParser PUBLIC
  ${EXPAT_LIBRARIES}
)
