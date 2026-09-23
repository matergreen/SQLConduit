cmake_minimum_required(VERSION 3.16)
project(sqlconduit_consumer_smoke LANGUAGES CXX)

option(SQLCONDUIT_CONSUMER_REQUIRE_DRIVERS
       "Require at least one built-in driver in the installed package" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(sqlconduit REQUIRED)

add_executable(sqlconduit_consumer_smoke main.cpp)
target_link_libraries(sqlconduit_consumer_smoke PRIVATE sqlconduit::sqlconduit)
if(SQLCONDUIT_CONSUMER_REQUIRE_DRIVERS)
    target_compile_definitions(sqlconduit_consumer_smoke
                               PRIVATE SQLCONDUIT_CONSUMER_REQUIRE_DRIVERS=1)
endif()
