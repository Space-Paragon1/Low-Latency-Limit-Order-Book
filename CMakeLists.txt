cmake_minimum_required(VERSION 3.15)
project(LimitOrderBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for benchmarks
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -O3 -march=native)
endif()

include_directories(include)

# ─── Core library ────────────────────────────────────────────────────────────
add_library(lob_core STATIC
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/MessageParser.cpp
    src/MarketDataPublisher.cpp
)

# ─── Main replay executable ───────────────────────────────────────────────────
add_executable(lob_main src/main.cpp)
target_link_libraries(lob_main PRIVATE lob_core)

# ─── Unit tests ───────────────────────────────────────────────────────────────
add_executable(lob_tests tests/test_engine.cpp)
target_link_libraries(lob_tests PRIVATE lob_core)

# ─── Benchmark harness ────────────────────────────────────────────────────────
add_executable(lob_bench benchmark/benchmark.cpp)
target_link_libraries(lob_bench PRIVATE lob_core)

# ─── CTest integration ────────────────────────────────────────────────────────
enable_testing()
add_test(NAME unit_tests COMMAND lob_tests)
