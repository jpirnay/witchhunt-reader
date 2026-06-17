# FileIndex: on-SD streaming directory index for large folders
idf_component_register(
  SRCS "FileIndex.cpp"
  INCLUDE_DIRS "."
  REQUIRES hal_storage fshelpers
)
