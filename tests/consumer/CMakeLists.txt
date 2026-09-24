cmake_minimum_required(VERSION 3.16)
project(sqlconduit_consumer_smoke LANGUAGES CXX)

set(SQLCONDUIT_CONSUMER_COMPONENT "Core" CACHE STRING
    "Installed SQLConduit component to exercise (Core/MySQL/Postgres/ODBC/Oracle)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(sqlconduit REQUIRED COMPONENTS ${SQLCONDUIT_CONSUMER_COMPONENT})
if(NOT DEFINED sqlconduit_SCHEMA_FILE OR NOT EXISTS "${sqlconduit_SCHEMA_FILE}")
    message(FATAL_ERROR "Installed SQLConduit package does not expose its JSON Schema")
endif()

add_executable(sqlconduit_consumer_smoke main.cpp)
string(TOLOWER "${SQLCONDUIT_CONSUMER_COMPONENT}" _component_target)
target_link_libraries(sqlconduit_consumer_smoke PRIVATE sqlconduit::${_component_target})
if(NOT SQLCONDUIT_CONSUMER_COMPONENT STREQUAL "Core")
    string(TOUPPER "${SQLCONDUIT_CONSUMER_COMPONENT}" _component_define)
    target_compile_definitions(sqlconduit_consumer_smoke PRIVATE
                               "SQLCONDUIT_CONSUMER_${_component_define}=1")
endif()
