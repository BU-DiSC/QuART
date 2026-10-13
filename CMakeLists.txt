cmake_minimum_required(VERSION 3.10)

project(QuART VERSION 1.0
        DESCRIPTION "QuART: Quick Ingestion to ART through Sortedness Awareness"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release only if no build type is set (and not a multi-config generator)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

# Allow selection in cache GUIs / command line
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

# ── Key-width selection ────────────────────────────────────────────────────────
# Set QUART_KEY_64=ON to build with 64-bit integer keys (default: 32-bit).
#   cmake -DQUART_KEY_64=ON ..
# NOTE: ART::bulkLoad is 32-bit-only and produces wrong results under 64-bit keys.
option(QUART_KEY_64 "Use 64-bit integer keys (default: 32-bit)" OFF)
if(QUART_KEY_64)
    message(STATUS "QuART: building with 64-bit keys")
    add_compile_definitions(QUART_KEY_64)
else()
    message(STATUS "QuART: building with 32-bit keys (default)")
endif()

# ── Concurrent (OLC) stat counters ──────────────────────────────────────────────
# Set QUART_CONC_STATS=ON to compile lock-contention / restart counters into
# test_conc_olc (both concurrent arms): how many times a descender had to
# restart, why, and how long it waited on a lock.  These add atomic increments
# to the insert hot path, so they are OFF by default to keep timing runs clean.
#   cmake -DQUART_CONC_STATS=ON ..
option(QUART_CONC_STATS "Compile lock-contention counters into test_conc_olc" OFF)
if(QUART_CONC_STATS)
    message(STATUS "QuART: conc lock counters ENABLED (test_conc_olc)")
else()
    message(STATUS "QuART: conc lock counters disabled (default)")
endif()

# Set QUART_STAIL_FP_STATS=ON to compile QuART_conc_olc_stail's fast-path
# effectiveness counters into test_conc_olc (printed only on the OLC+stail-fp
# arm): the per-key classification histogram, the fast-append hit rate, and the
# fast-append contention rate.  Like QUART_CONC_STATS these add atomics to the
# insert hot path, so they are OFF by default; enable them for a counts-only
# build and take timings from a stats-OFF build.
#   cmake -DQUART_STAIL_FP_STATS=ON ..
option(QUART_STAIL_FP_STATS "Compile stail fast-path effectiveness counters into test_conc_olc" OFF)
if(QUART_STAIL_FP_STATS)
    message(STATUS "QuART: stail fast-path counters ENABLED (test_conc_olc)")
else()
    message(STATUS "QuART: stail fast-path counters disabled (default)")
endif()

# ── Targets ─────────────────────────────────────────────────────────────────────
# All executables are tuned for the build host with -march=native.  This enables
# the wider instruction set the Node16 SSE compares benefit from.  Note: it
# applies to the WHOLE binary, so the result may SIGILL on an older/different CPU.
#
# Note: QuArtNodeMethods.cpp / QuArtNodeBulkLoadMethods.cpp are #include'd from the
# QuART headers, so they must NOT be listed as separate translation units here.

find_package(Threads REQUIRED)

# Main serial benchmark driver: ART / QuART_tail / QuART_lil / QuART_stail,
# plus the --bulkload (ART grouped bulk loading) and --synthetic paths.
# Backs §4.1 (BoDS), §4.2 (TPC-H), and §4.3 (bulk loading).
add_executable(run run.cpp)

# §4.4 (Fig 14): concurrent insert throughput vs thread count.  A/Bs the two
# concurrent tree types on identical multi-stream workloads —
# QuART_conc_olc_art (ART-OLC, no fast path) vs QuART_conc_olc_stail (per-thread
# stail fast-path tail) — both built on the shared OLC base in trees/olc_base.h.
add_executable(test_conc_olc test_conc_olc.cpp)
target_link_libraries(test_conc_olc PRIVATE Threads::Threads)
if(QUART_CONC_STATS)
    target_compile_definitions(test_conc_olc PRIVATE QUART_CONC_STATS)
endif()
if(QUART_STAIL_FP_STATS)
    target_compile_definitions(test_conc_olc PRIVATE QUART_STAIL_FP_STATS)
endif()

# §4.4 (Fig 15): mixed concurrent read/write.  A/Bs QuART_conc_olc_art vs
# QuART_conc_olc_stail under a mix of lock-free lookups (olcLookup) + near-sorted
# inserts across read ratios and thread counts.
add_executable(test_conc_olc_readwrite test_conc_olc_readwrite.cpp)
target_link_libraries(test_conc_olc_readwrite PRIVATE Threads::Threads)

foreach(target run test_conc_olc test_conc_olc_readwrite)
    target_compile_options(${target} PRIVATE -march=native)
endforeach()
