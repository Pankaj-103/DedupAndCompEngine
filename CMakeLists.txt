cmake_minimum_required(VERSION 3.24)
project(DedupAndCompEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.7
  SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

FetchContent_Declare(blake3
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
  GIT_TAG 1.8.2
  SOURCE_SUBDIR c
)
FetchContent_MakeAvailable(blake3)

add_subdirectory(third_party/sqlite)
add_subdirectory(engine)
