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
  src/lib/rdb.c
  src/lib/translate.c

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
  test_rdb
  test_translate


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
add_executable(m7_rdb tests/oracle/m7_rdb.c)
target_link_libraries(m7_rdb PRIVATE amigafs_core)
add_executable(m10_partition tests/oracle/m10_partition.c)
target_link_libraries(m10_partition PRIVATE amigafs_core)

# --------------------------------------------------------------- FUSE mount ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
add_executable(amigafs-fuse src/fuse/main.c)
target_link_libraries(amigafs-fuse PRIVATE amigafs_core)
target_compile_options(amigafs-fuse PRIVATE -DFUSE_USE_VERSION=31 ${FUSE3_CFLAGS_OTHER})
target_include_directories(amigafs-fuse PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_directories(amigafs-fuse PRIVATE ${FUSE3_LIBRARY_DIRS})
target_link_libraries(amigafs-fuse PRIVATE ${FUSE3_LIBRARIES})

# ------------------------------------------------------------------- CLI ----
add_executable(afs src/cli/main.c)
target_link_libraries(afs PRIVATE amigafs_core)

# ------------------------------------------- verifier + robustness gates ----
# m6_damage_probe is the NEGATIVE CONTROL for the structure verifier: it
# damages each reachable block and requires the verifier to name it. Without it
# a verifier that never descends past the rootblock still "passes" by agreeing
# with ADFlib on the hostile disk.
add_executable(m6_damage_probe tests/oracle/m6_damage_probe.c)
target_link_libraries(m6_damage_probe PRIVATE amigafs_core)

# ------------------------------------------------- oracle / integration gates ----
# These compare the core library against pinned external oracles (ADFlib CLI,
# amitools) and, for M8, against a real FUSE mount. They need the fixtures and
# the oracles present, so they are registered but skipped cleanly when not.
set(AMIGAFS_GATES m0_gate m2_gate m3_gate m4_gate m5_gate m6_gate m7_gate m8_gate m10_gate fuzz_gate)
foreach(g ${AMIGAFS_GATES})
  add_test(NAME gate_${g}
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/${g}.sh)
  set_tests_properties(gate_${g} PROPERTIES TIMEOUT 300)
endforeach()

# the end-to-end harness as a single ctest entry
add_test(NAME all_gates
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_all_gates.sh)
set_tests_properties(all_gates PROPERTIES TIMEOUT 900)

enable_testing()
