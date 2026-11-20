cmake_minimum_required(VERSION 3.16)
project(LimitOrderBook VERSION 2.0.0 LANGUAGES CXX)

# ─── Options ─────────────────────────────────────────────────────────────────
set(LOB_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(LOB_TOP_LEVEL ON)
endif()

option(LOB_BUILD_TESTS           "Build the test suite"                       ${LOB_TOP_LEVEL})
option(LOB_BUILD_BENCHMARKS      "Build the benchmark harness"                ${LOB_TOP_LEVEL})
option(LOB_BUILD_APPS            "Build the lob_main replay tool"             ${LOB_TOP_LEVEL})
option(LOB_ENABLE_SANITIZERS     "Instrument with AddressSanitizer + UBSan"   OFF)
option(LOB_ENABLE_LTO            "Enable link-time optimisation"              OFF)
option(LOB_ENABLE_NATIVE         "Tune for the build machine (-march=native)" OFF)
option(LOB_WARNINGS_AS_ERRORS    "Treat compiler warnings as errors"          OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Shared compile settings (interface target) ──────────────────────────────
add_library(lob_options INTERFACE)
target_compile_features(lob_options INTERFACE cxx_std_17)

if(MSVC)
    # C4324: padding added by alignas (OrderNode is cache-line aligned on purpose)
    target_compile_options(lob_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /wd4324)
    target_compile_definitions(lob_options INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(LOB_WARNINGS_AS_ERRORS)
        target_compile_options(lob_options INTERFACE /WX)
    endif()
else()
    target_compile_options(lob_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference)
    if(LOB_WARNINGS_AS_ERRORS)
        target_compile_options(lob_options INTERFACE -Werror)
    endif()
    if(LOB_ENABLE_SANITIZERS)
        target_compile_options(lob_options INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(lob_options INTERFACE -fsanitize=address,undefined)
    endif()
    if(LOB_ENABLE_NATIVE)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-march=native LOB_HAS_MARCH_NATIVE)
        check_cxx_compiler_flag(-mcpu=native LOB_HAS_MCPU_NATIVE)
        if(LOB_HAS_MARCH_NATIVE)
            target_compile_options(lob_options INTERFACE -march=native)
        elseif(LOB_HAS_MCPU_NATIVE)
            target_compile_options(lob_options INTERFACE -mcpu=native)
        endif()
    endif()
endif()

if(LOB_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LOB_IPO_OK OUTPUT LOB_IPO_MSG)
    if(LOB_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO requested but not supported: ${LOB_IPO_MSG}")
    endif()
endif()

# ─── Core library ────────────────────────────────────────────────────────────
add_library(lob_core STATIC
    src/Types.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/MessageParser.cpp
    src/MarketDataPublisher.cpp
)
add_library(lob::core ALIAS lob_core)
target_include_directories(lob_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lob_core PUBLIC lob_options)

# ─── Replay tool ─────────────────────────────────────────────────────────────
if(LOB_BUILD_APPS)
    add_executable(lob_main src/main.cpp)
    target_link_libraries(lob_main PRIVATE lob_core)
endif()

# ─── Benchmark ───────────────────────────────────────────────────────────────
if(LOB_BUILD_BENCHMARKS)
    add_executable(lob_bench benchmark/benchmark.cpp)
    target_link_libraries(lob_bench PRIVATE lob_core)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
if(LOB_BUILD_TESTS)
    enable_testing()

    add_executable(lob_tests
        tests/main.cpp
        tests/test_order_book.cpp
        tests/test_stops.cpp
        tests/test_parser.cpp
        tests/test_engine.cpp
        tests/test_fuzz.cpp
    )
    target_link_libraries(lob_tests PRIVATE lob_core)

    add_test(NAME unit_tests COMMAND lob_tests)

    if(LOB_BUILD_APPS)
        add_test(NAME golden_replay
            COMMAND ${CMAKE_COMMAND}
                -DLOB_MAIN=$<TARGET_FILE:lob_main>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/sample_orders.csv
                -DEXPECTED_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
                -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/golden_out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GoldenTest.cmake)
    endif()

    if(LOB_BUILD_BENCHMARKS)
        add_test(NAME bench_smoke COMMAND lob_bench --events 50000 --repeat 1)
    endif()
endif()
