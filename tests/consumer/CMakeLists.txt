cmake_minimum_required(VERSION 3.16)
project(sqlconduit_consumer_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(sqlconduit REQUIRED)

add_executable(sqlconduit_consumer_smoke main.cpp)
target_link_libraries(sqlconduit_consumer_smoke PRIVATE sqlconduit::sqlconduit)
