cmake_minimum_required(VERSION 3.16)
project(zgec C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Pedantic, maximally warning-heavy build.
add_compile_options(
    -pedantic
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
    -Wundef
    -Wnull-dereference
    -Wdouble-promotion
    -O3
)

# Reference platform: x86-64 with AVX2 and BMI2.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_compile_options(-mavx2 -mbmi2 -msse4.2)
endif()

# Core library: every module.
set(ZGEC_SOURCES
    src/common.c
    src/bitstream.c
    src/crc32c.c
    src/xxhash.c
    src/fse.c
    src/rans.c
    src/frame.c
    src/dict.c
    src/seq.c
    src/lit.c
    src/block.c
    src/decode.c
    src/match.c
    src/parse.c
    src/encode.c
)

find_package(Threads REQUIRED)

add_library(zgec_core STATIC ${ZGEC_SOURCES})
target_include_directories(zgec_core PUBLIC include)
target_link_libraries(zgec_core PUBLIC Threads::Threads m)

# Command-line tool.
add_executable(zgec src/cli.c)
target_link_libraries(zgec PRIVATE zgec_core)

# Conformance / round-trip test harness.
add_executable(zgec_test tests/test.c)
target_link_libraries(zgec_test PRIVATE zgec_core)

# Minimal smoke test (frame/FSE/rANS/round-trip).
add_executable(zgec_test_mini tests/test_mini.c)
target_link_libraries(zgec_test_mini PRIVATE zgec_core)

# Block-parameter / segment-header / bitstream checks.
add_executable(zgec_test_block tests/test_block_check.c)
target_link_libraries(zgec_test_block PRIVATE zgec_core)

# Microbenchmark.
add_executable(zgec_bench tests/bench.c)
target_link_libraries(zgec_bench PRIVATE zgec_core)
