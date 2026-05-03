cmake_minimum_required(VERSION 3.14)
project(MPSC_Project CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(DEFAULT_RELEASE_OPTS "$<$<CXX_COMPILER_ID:MSVC>:/O2 /Ob2>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>")
endif()

include(FetchContent)

# Google Benchmark
FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    FIND_PACKAGE_ARGS NAMES benchmark CONFIG
)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

if(TARGET benchmark)
    target_compile_options(benchmark PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions -Wno-invalid-offsetof>
    )
endif()
if(TARGET benchmark_main)
    target_compile_options(benchmark_main PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions -Wno-invalid-offsetof>
    )
endif()

# Google Test
enable_testing()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    FIND_PACKAGE_ARGS NAMES GTest CONFIG
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# HdrHistogram
FetchContent_Declare(
    hdrhistogram
    URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.8.zip
)
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(HDR_LOG_REQUIRED "DISABLED" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(hdrhistogram)

# moodycamel ConcurrentQueue
FetchContent_Declare(
    concurrentqueue
    URL https://github.com/cameron314/concurrentqueue/archive/refs/tags/v1.0.5.zip
)
FetchContent_MakeAvailable(concurrentqueue)

find_package(Threads REQUIRED)

set(ATOMIC_LIBRARY
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<NOT:$<PLATFORM_ID:Windows>>>:atomic>
)

set(COMMON_TARGET_PROPERTIES
    PRIVATE
        ${DEFAULT_RELEASE_OPTS}
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)

#BENCHMARK
add_executable(mpsc_bench_throughput benchmarks/bench_throughput.cpp)
target_include_directories(mpsc_bench_throughput
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_bench_throughput
    PRIVATE
        benchmark::benchmark_main
        Threads::Threads
        ${ATOMIC_LIBRARY}
)
target_compile_options(mpsc_bench_throughput ${COMMON_TARGET_PROPERTIES})

add_executable(mpsc_bench_latency benchmarks/bench_latency.cpp)
target_include_directories(mpsc_bench_latency 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${hdrhistogram_SOURCE_DIR}/src 
)
target_link_libraries(mpsc_bench_latency
    PRIVATE
        benchmark::benchmark_main
        hdr_histogram_static 
        Threads::Threads
        ${ATOMIC_LIBRARY}
)
target_compile_options(mpsc_bench_latency ${COMMON_TARGET_PROPERTIES})

add_executable(mpsc_bench_linearizable benchmarks/bench_linearizable.cpp)
target_include_directories(mpsc_bench_linearizable 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_bench_linearizable
    PRIVATE
        benchmark::benchmark_main
        Threads::Threads
        ${ATOMIC_LIBRARY}
)
target_compile_options(mpsc_bench_linearizable ${COMMON_TARGET_PROPERTIES})

add_executable(mpsc_vs_mpmc_benchmark benchmarks/bench_mpsc_vs_mpmc_google.cpp)
target_include_directories(mpsc_vs_mpmc_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
)
target_link_libraries(mpsc_vs_mpmc_benchmark
    PRIVATE
        benchmark::benchmark_main
        Threads::Threads
        ${ATOMIC_LIBRARY}
)
target_compile_options(mpsc_vs_mpmc_benchmark ${COMMON_TARGET_PROPERTIES})

# TEST
add_executable(mpsc_tests tests/test_MPSC.cpp)
target_include_directories(mpsc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpsc_tests PRIVATE GTest::gtest_main GTest::gmock Threads::Threads ${ATOMIC_LIBRARY})
include(GoogleTest)
gtest_discover_tests(mpsc_tests)

#EXAMPLE
add_executable(mpsc_log_system_example examples/log_system.cpp)
target_include_directories(mpsc_log_system_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpsc_log_system_example PRIVATE Threads::Threads ${ATOMIC_LIBRARY})
target_compile_options(mpsc_log_system_example ${COMMON_TARGET_PROPERTIES})

add_executable(mpsc_command_dispatcher_example examples/command_dispatcher.cpp)
target_include_directories(mpsc_command_dispatcher_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpsc_command_dispatcher_example PRIVATE Threads::Threads ${ATOMIC_LIBRARY})
target_compile_options(mpsc_command_dispatcher_example ${COMMON_TARGET_PROPERTIES})
