add_executable(dedupc
  src/main.cpp
  src/db.cpp
  src/hash.cpp
  src/codec.cpp
  src/repo.cpp
  src/commands.cpp
)

target_link_libraries(dedupc PRIVATE libzstd_static blake3 sqlite3)
target_include_directories(dedupc PRIVATE
  ${zstd_SOURCE_DIR}/lib
  ${blake3_SOURCE_DIR}/c
)

if(MSVC)
  target_compile_options(dedupc PRIVATE /W4)
else()
  target_compile_options(dedupc PRIVATE -Wall -Wextra)
endif()

# Self-contained binary: no dependency on compiler runtime DLLs.
if(MINGW)
  target_link_options(dedupc PRIVATE -static)
endif()
