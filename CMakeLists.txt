cmake_minimum_required(VERSION 3.14)
project(MPSC_Project CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)


if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(DEFAULT_RELEASE_OPTS "$<$<CXX_COMPILER_ID:MSVC>:/O2 /Ob2> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>")
endif()

include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    FIND_PACKAGE_ARGS NAMES benchmark CONFIG
)
set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_MakeAvailable(googlebenchmark)

find_package(Threads REQUIRED)

add_executable(mpsc_bench benchmarks/bench.cpp)

target_include_directories(mpsc_bench PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR} 
)

target_link_libraries(mpsc_bench PRIVATE 
    benchmark::benchmark_main
    Threads::Threads
)

target_compile_options(mpsc_bench PRIVATE ${DEFAULT_RELEASE_OPTS})


add_executable(mpsc_log_example "examples/log_system.cpp")

target_include_directories(mpsc_log_example PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mpsc_log_example PRIVATE 
    Threads::Threads
)

target_compile_options(mpsc_log_example PRIVATE ${DEFAULT_RELEASE_OPTS})


add_executable(mpsc_command_dispatcher examples/command_dispatcher.cpp)

target_include_directories(mpsc_command_dispatcher PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mpsc_command_dispatcher PRIVATE 
    Threads::Threads
)

target_compile_options(mpsc_command_dispatcher PRIVATE ${DEFAULT_RELEASE_OPTS})