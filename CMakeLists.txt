cmake_minimum_required(VERSION 3.25)
project(dr_dict VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include(FetchContent)
include(CTest)
# Third-party projects such as miniz still declare older CMake policy versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
endif()
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json miniz)
FetchContent_Declare(sqlite URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite)
add_library(dr_sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(dr_sqlite PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(dr_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION)
# OpenCC (Apache-2.0) converts between simplified and traditional Chinese for variant lookups.
# Its build compiles the .ocd2 dictionaries from text sources, which needs Python 3.
FetchContent_Declare(opencc URL https://github.com/BYVoid/OpenCC/archive/refs/tags/ver.1.4.2.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(ENABLE_GTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(BUILD_OPENCC_JIEBA_PLUGIN OFF CACHE BOOL "" FORCE)
set(OPENCC_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opencc)
# Configs and compiled dictionaries sit side by side in bin/<config>/opencc; a config names its
# dictionaries relative to its own folder.
set(DR_OPENCC_CONFIGS s2t s2tw s2twp s2hk t2s tw2s tw2sp hk2s)
set(DR_OPENCC_COPY "")
foreach(config ${DR_OPENCC_CONFIGS})
  list(APPEND DR_OPENCC_COPY "${opencc_SOURCE_DIR}/data/config/${config}.json")
endforeach()
add_custom_target(dr-opencc-data
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/opencc"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DR_OPENCC_COPY} "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/opencc"
  COMMAND ${CMAKE_COMMAND} "-DSOURCE=${opencc_BINARY_DIR}/data" "-DDEST=${CMAKE_BINARY_DIR}/bin/$<CONFIG>/opencc"
    -P "${PROJECT_SOURCE_DIR}/cmake/copy_ocd2.cmake"
  COMMENT "Copying OpenCC configs and dictionaries"
  VERBATIM)
add_dependencies(dr-opencc-data Dictionaries)
add_subdirectory(engine)
if(WIN32)
  add_subdirectory(ui)
  add_subdirectory(app)
endif()
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
