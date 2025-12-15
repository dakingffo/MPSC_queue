cmake_minimum_required(VERSION 3.14)
project(MPSC_Project CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(DEFAULT_RELEASE_OPTS "$<$<CXX_COMPILER_ID:MSVC>:/O2 /Ob2>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>")
endif()

include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    FIND_PACKAGE_ARGS NAMES benchmark CONFIG
)
set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_MakeAvailable(googlebenchmark)

enable_testing()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    FIND_PACKAGE_ARGS NAMES GTest CONFIG
)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)

# 通用编译选项定义
set(COMMON_TARGET_PROPERTIES
    PRIVATE
        ${DEFAULT_RELEASE_OPTS}
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)

# 基准测试可执行文件
add_executable(mpsc_bench benchmarks/bench.cpp)
target_include_directories(mpsc_bench 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_bench 
    PRIVATE
        benchmark::benchmark_main
        Threads::Threads
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:atomic>
)
target_compile_options(mpsc_bench ${COMMON_TARGET_PROPERTIES})

# 单元测试可执行文件
enable_testing()
add_executable(mpsc_tests tests/test_MPSC.cpp)
target_include_directories(mpsc_tests 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_tests
    PRIVATE
        GTest::gtest_main 
        GTest::gmock         
        Threads::Threads
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:atomic>
)
include(GoogleTest)
gtest_discover_tests(mpsc_tests)

# 日志系统示例
add_executable(mpsc_log_system_example "examples/log_system.cpp")
target_include_directories(mpsc_log_system_example 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_log_system_example 
    PRIVATE
        Threads::Threads
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:atomic>
)
target_compile_options(mpsc_log_system_example ${COMMON_TARGET_PROPERTIES})

# 命令分发器示例
add_executable(mpsc_command_dispatcher_example examples/command_dispatcher.cpp)
target_include_directories(mpsc_command_dispatcher_example
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mpsc_command_dispatcher_example
    PRIVATE
        Threads::Threads
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:atomic>
)
target_compile_options(mpsc_command_dispatcher_example ${COMMON_TARGET_PROPERTIES})