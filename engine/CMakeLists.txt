add_library(dr_engine STATIC
  src/paths.cpp src/text.cpp src/compress.cpp src/ripemd128.cpp
  src/mdict.cpp src/stardict.cpp src/library.cpp)
target_include_directories(dr_engine PUBLIC include)
# Keep miniz from defining zlib-style macros such as adler32 that collide with engine names.
target_compile_definitions(dr_engine PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(dr_engine PUBLIC nlohmann_json::nlohmann_json PRIVATE miniz dr_sqlite normaliz)
if(WIN32)
  target_compile_definitions(dr_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
