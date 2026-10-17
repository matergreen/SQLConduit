cmake_minimum_required(VERSION 3.16)
project(sqlconduit_benchmark_consumer LANGUAGES CXX)

# Build this exact harness twice against separately installed library versions.
# Different historical main() bodies can change GCC inlining/code layout and
# produce apparent library regressions even on the same compiler and host.
find_package(sqlconduit REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)
add_executable(sqlconduit_benchmark ../sqlconduit_benchmark.cpp ../observer_workload.cpp ../cache_workload.cpp)
set_target_properties(sqlconduit_benchmark PROPERTIES
                      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(sqlconduit_benchmark PRIVATE sqlconduit::core Threads::Threads)
