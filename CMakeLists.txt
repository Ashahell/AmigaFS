cmake_minimum_required(VERSION 3.16)
project(amigafs C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)

# ---------------------------------------------------------------- core lib ----
# Platform-independent. No FUSE, no OS calls except through blockio's pread backend.
add_library(amigafs_core STATIC
  src/lib/be.c
  src/lib/checksum.c
  src/lib/hash.c
  src/lib/names.c
  src/lib/blockio.c
  src/lib/blocktypes.c
  src/lib/volume.c
  src/lib/dir.c
  src/lib/file.c
  src/lib/links.c
  src/lib/bitmap.c
  src/lib/verify.c

)
target_include_directories(amigafs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --------------------------------------------------------------- unit tests ----
enable_testing()

set(AMIGAFS_UNIT_TESTS
  test_be
  test_checksum
  test_hash
  test_blockio
  test_blocktypes
  test_volume
  test_dir
  test_file
  test_links
  test_verify


)
foreach(t ${AMIGAFS_UNIT_TESTS})
  add_executable(${t} tests/unit/${t}.c)
  target_link_libraries(${t} PRIVATE amigafs_core)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# ------------------------------------------------- integration (needs fixtures) ----
add_executable(m1_integration tests/oracle/m1_integration.c)
target_link_libraries(m1_integration PRIVATE amigafs_core)
add_executable(m2_gate tests/oracle/m2_gate.c)
target_link_libraries(m2_gate PRIVATE amigafs_core)
add_executable(m3_ls tests/oracle/m3_ls.c)
target_link_libraries(m3_ls PRIVATE amigafs_core)
add_executable(m4_cat tests/oracle/m4_cat.c)
target_link_libraries(m4_cat PRIVATE amigafs_core)
add_executable(m5_links tests/oracle/m5_links.c)
target_link_libraries(m5_links PRIVATE amigafs_core)
add_executable(m6_verify tests/oracle/m6_verify.c)
target_link_libraries(m6_verify PRIVATE amigafs_core)
