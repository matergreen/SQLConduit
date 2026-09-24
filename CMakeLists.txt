cmake_minimum_required(VERSION 3.16)
project(sqlconduit VERSION 0.8.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/public_api.cmake")
file(GLOB_RECURSE _sqlconduit_header_inventory CONFIGURE_DEPENDS
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/sqlconduit/*.h")
list(REMOVE_ITEM _sqlconduit_header_inventory
     "sqlconduit/async/executor.h"
     "sqlconduit/core/runtime_services.h"
     "sqlconduit/core/stats_reporter.h"
     "sqlconduit/driver/mysql_driver.h"
     "sqlconduit/driver/postgres_driver.h"
     "sqlconduit/driver/odbc_driver.h"
     "sqlconduit/driver/oracle_driver.h")
list(SORT _sqlconduit_header_inventory)
set(_sqlconduit_frozen_headers "${SQLCONDUIT_FROZEN_PUBLIC_HEADERS}")
list(SORT _sqlconduit_frozen_headers)
if(NOT _sqlconduit_header_inventory STREQUAL _sqlconduit_frozen_headers)
    message(FATAL_ERROR
        "Installed public headers differ from cmake/public_api.cmake. "
        "Public API changes must update the frozen inventory and changelog.\n"
        "Expected: ${_sqlconduit_frozen_headers}\n"
        "Actual:   ${_sqlconduit_header_inventory}")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/sqlconduit/version.h"
     _sqlconduit_version_header)
foreach(_part MAJOR MINOR PATCH)
    if(NOT _sqlconduit_version_header MATCHES
       "#define SQLCONDUIT_VERSION_${_part} ${PROJECT_VERSION_${_part}}([^0-9]|$)")
        message(FATAL_ERROR
            "include/sqlconduit/version.h does not match project version ${PROJECT_VERSION}")
    endif()
endforeach()
if(NOT _sqlconduit_version_header MATCHES
   "#define SQLCONDUIT_VERSION_STRING \"${PROJECT_VERSION}\"")
    message(FATAL_ERROR
        "SQLCONDUIT_VERSION_STRING does not match project version ${PROJECT_VERSION}")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SQLCONDUIT_ENABLE_MYSQL    "Build MySQL driver (needs libmysqlclient)"      OFF)
option(SQLCONDUIT_ENABLE_POSTGRES "Build PostgreSQL driver (needs libpqxx + libpq)" OFF)
option(SQLCONDUIT_ENABLE_ODBC     "Build ODBC driver for SQL Server/Oracle"         OFF)
option(SQLCONDUIT_ENABLE_ORACLE   "Build Oracle driver (needs OCI / Instant Client)" OFF)
option(SQLCONDUIT_BUILD_TESTS     "Build core behaviour tests (no external deps)"   OFF)
option(SQLCONDUIT_BUILD_BENCHMARKS "Build dependency-free performance benchmarks"   OFF)
option(SQLCONDUIT_INSTALL         "Generate install and package config rules"       ON)
option(SQLCONDUIT_BUILD_INTEGRATION_TESTS
       "Build integration tests that require live database services"          OFF)

set(_nl_inc_dir "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    set(_nl_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    message(STATUS "sqlconduit: using vendored nlohmann/json")
else()
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND AND TARGET nlohmann_json::nlohmann_json)
        get_target_property(_nl_inc_dir nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
        message(STATUS "sqlconduit: using system nlohmann_json")
    else()
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3)
        FetchContent_MakeAvailable(nlohmann_json)
        set(_nl_inc_dir "${nlohmann_json_SOURCE_DIR}/single_include")
        message(STATUS "sqlconduit: fetched nlohmann_json via FetchContent")
    endif()
endif()

set(SQLCONDUIT_CORE_SOURCES
    src/common/types.cpp
    src/common/pg_types.cpp
    src/common/oracle_types.cpp
    src/common/context.cpp
    src/common/observer.cpp
    src/common/sql_analyze.cpp
    src/config/datasource_config.cpp
    src/config/config_loader.cpp
    src/config/yaml_parser.cpp
    src/async/executor.cpp
    src/core/idatabase_connection.cpp
    src/core/connection_pool.cpp
    src/core/heartbeat_manager.cpp
    src/core/database_manager.cpp
    src/core/sql_auditor.cpp
    src/core/query_cache.cpp
    src/core/runtime_services.cpp
    src/core/write_buffer.cpp
    src/core/stats_reporter.cpp
    src/core/interceptor.cpp
    src/driver/driver_registry.cpp
    src/client.cpp
    src/exporters/prometheus.cpp
)

add_library(sqlconduit_core STATIC ${SQLCONDUIT_CORE_SOURCES})
add_library(sqlconduit::core ALIAS sqlconduit_core)
set_target_properties(sqlconduit_core PROPERTIES EXPORT_NAME core)
target_include_directories(sqlconduit_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${_nl_inc_dir})

# The library owns worker threads, so static consumers need the thread flag too.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(sqlconduit_core PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

# Each optional driver is its own static component. Its database client dependency
# stays off the core target and is re-discovered only when a consumer requests that
# component from the installed package.
set(SQLCONDUIT_INSTALL_TARGETS sqlconduit_core)
set(SQLCONDUIT_BUILT_COMPONENTS Core)

function(sqlconduit_add_driver_target name source)
    string(TOLOWER "${name}" _lower)
    set(_target "sqlconduit_${_lower}")
    add_library(${_target} STATIC "${source}")
    add_library("sqlconduit::${_lower}" ALIAS ${_target})
    set_target_properties(${_target} PROPERTIES EXPORT_NAME "${_lower}")
    target_include_directories(${_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
    target_link_libraries(${_target} PUBLIC sqlconduit_core)
    list(APPEND SQLCONDUIT_INSTALL_TARGETS ${_target})
    list(APPEND SQLCONDUIT_BUILT_COMPONENTS ${name})
    set(SQLCONDUIT_INSTALL_TARGETS "${SQLCONDUIT_INSTALL_TARGETS}" PARENT_SCOPE)
    set(SQLCONDUIT_BUILT_COMPONENTS "${SQLCONDUIT_BUILT_COMPONENTS}" PARENT_SCOPE)
endfunction()

function(sqlconduit_add_driver_dep target name)
    foreach(_lib IN LISTS ARGN)
        target_link_libraries(${target} PRIVATE "$<BUILD_INTERFACE:${_lib}>")
        get_filename_component(_pcname "${_lib}" NAME_WE)
        string(REGEX REPLACE "^lib" "" _pcname "${_pcname}")
        set_property(GLOBAL APPEND PROPERTY
                     "SQLCONDUIT_${name}_PC_LIBS_PRIVATE" "-l${_pcname}")
    endforeach()
endfunction()

if(SQLCONDUIT_ENABLE_MYSQL)
    sqlconduit_add_driver_target(MySQL src/driver/mysql_driver.cpp)
    target_compile_definitions(sqlconduit_mysql PRIVATE SQLCONDUIT_ENABLE_MYSQL)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
    find_library(MYSQL_LIBRARY  NAMES mysqlclient libmysql)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_MYSQL=ON but libmysqlclient not found. "
            "Run: sudo apt install -y default-libmysqlclient-dev")
    endif()
    target_include_directories(sqlconduit_mysql PRIVATE ${MYSQL_INCLUDE_DIR})
    sqlconduit_add_driver_dep(sqlconduit_mysql MYSQL "${MYSQL_LIBRARY}")
endif()

if(SQLCONDUIT_ENABLE_POSTGRES)
    sqlconduit_add_driver_target(Postgres src/driver/postgres_driver.cpp)
    target_compile_definitions(sqlconduit_postgres PRIVATE SQLCONDUIT_ENABLE_POSTGRES)
    find_path(PQXX_INCLUDE_DIR NAMES pqxx/pqxx)
    if(UNIX AND NOT APPLE)
        # Prefer static libpqxx on Linux. pqxx::internal::type_name is a C++17 inline
        # template variable; shared linking may create one instance in the executable and
        # another in libpqxx.so, registering two destructors and causing a double free at
        # process exit. Static linking coalesces it into one instance. Fall back if needed.
        find_library(PQXX_LIBRARY NAMES libpqxx.a pqxx)
    else()
        find_library(PQXX_LIBRARY NAMES pqxx)
    endif()
    find_path(PQ_INCLUDE_DIR    NAMES postgresql/libpq-fe.h libpq-fe.h)
    find_library(PQ_LIBRARY     NAMES pq)
    if(NOT PQXX_INCLUDE_DIR OR NOT PQXX_LIBRARY OR NOT PQ_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_POSTGRES=ON but libpqxx/libpq not found. "
            "Run: sudo apt install -y libpqxx-dev libpq-dev")
    endif()

    file(STRINGS "${PQXX_INCLUDE_DIR}/pqxx/version.hxx" _sqlconduit_pqxx_major_line
         REGEX "^#[ ]*define[ ]+PQXX_VERSION_MAJOR[ ]+[0-9]+")
    string(REGEX MATCH "([0-9]+)" _sqlconduit_pqxx_major "${_sqlconduit_pqxx_major_line}")
    if(NOT _sqlconduit_pqxx_major)
        message(WARNING "sqlconduit: cannot parse PQXX_VERSION_MAJOR from "
                        "${PQXX_INCLUDE_DIR}/pqxx/version.hxx; building as C++17.")
        set(_sqlconduit_pqxx_major 7)
    endif()
    if(_sqlconduit_pqxx_major LESS 7)
        message(FATAL_ERROR "SQLConduit requires libpqxx >= 7; detected major version "
            "${_sqlconduit_pqxx_major}. Version 6.x lacks pqxx::zview / pqxx::params. Use "
            "Ubuntu 24.04+, Homebrew libpqxx, or set -DSQLCONDUIT_ENABLE_POSTGRES=OFF.")
    endif()
    message(STATUS "sqlconduit: libpqxx major version ${_sqlconduit_pqxx_major}")
    if(_sqlconduit_pqxx_major GREATER_EQUAL 8)
        message(STATUS "sqlconduit: libpqxx >= 8 requires C++20; raising only the PostgreSQL driver target")
        set_target_properties(sqlconduit_postgres PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()

    target_include_directories(sqlconduit_postgres PRIVATE ${PQXX_INCLUDE_DIR} ${PQ_INCLUDE_DIR})
    sqlconduit_add_driver_dep(sqlconduit_postgres POSTGRES "${PQXX_LIBRARY}" "${PQ_LIBRARY}")
endif()

if(SQLCONDUIT_ENABLE_ODBC)
    sqlconduit_add_driver_target(ODBC src/driver/odbc_driver.cpp)
    target_compile_definitions(sqlconduit_odbc PRIVATE SQLCONDUIT_ENABLE_ODBC)
    if(WIN32)
        set(ODBC_LIBRARY odbc32)
    else()
        find_path(ODBC_INCLUDE_DIR NAMES sql.h)
        find_library(ODBC_LIBRARY NAMES odbc)
        if(NOT ODBC_INCLUDE_DIR OR NOT ODBC_LIBRARY)
            message(FATAL_ERROR "SQLCONDUIT_ENABLE_ODBC=ON but unixODBC not found. "
                "Run: sudo apt install -y unixodbc-dev")
        endif()
        target_include_directories(sqlconduit_odbc PRIVATE ${ODBC_INCLUDE_DIR})
    endif()
    sqlconduit_add_driver_dep(sqlconduit_odbc ODBC "${ODBC_LIBRARY}")
endif()

if(SQLCONDUIT_ENABLE_ORACLE)
    sqlconduit_add_driver_target(Oracle src/driver/oracle_driver.cpp)
    target_compile_definitions(sqlconduit_oracle PRIVATE SQLCONDUIT_ENABLE_ORACLE)
    find_path(OCI_INCLUDE_DIR NAMES oci.h)
    find_library(OCI_LIBRARY  NAMES clntsh oci)
    if(NOT OCI_INCLUDE_DIR OR NOT OCI_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_ORACLE=ON but the Oracle client was not found "
            "(oci.h / libclntsh). Install the Oracle Instant Client Basic + SDK packages and "
            "pass -DOCI_INCLUDE_DIR=<dir> -DOCI_LIBRARY=<path>.")
    endif()
    target_include_directories(sqlconduit_oracle PRIVATE ${OCI_INCLUDE_DIR})
    sqlconduit_add_driver_dep(sqlconduit_oracle ORACLE "${OCI_LIBRARY}")
endif()

if(SQLCONDUIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SQLCONDUIT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(NOT SQLCONDUIT_INSTALL)
    return()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${SQLCONDUIT_INSTALL_TARGETS}
        EXPORT sqlconduitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/sqlconduit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "async/executor.h" EXCLUDE
        PATTERN "core/runtime_services.h" EXCLUDE
        PATTERN "core/stats_reporter.h" EXCLUDE
        PATTERN "driver/mysql_driver.h" EXCLUDE
        PATTERN "driver/postgres_driver.h" EXCLUDE
        PATTERN "driver/odbc_driver.h" EXCLUDE
        PATTERN "driver/oracle_driver.h" EXCLUDE)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/sqlconduit.schema.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/sqlconduit")

set(_sqlconduit_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/sqlconduit")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sqlconduitConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfig.cmake"
    INSTALL_DESTINATION "${_sqlconduit_cmake_dir}"
    PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfigVersion.cmake"
    DESTINATION "${_sqlconduit_cmake_dir}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sqlconduitDriverDeps.cmake"
        DESTINATION "${_sqlconduit_cmake_dir}")
install(EXPORT sqlconduitTargets
        NAMESPACE sqlconduit::
        DESTINATION "${_sqlconduit_cmake_dir}")

if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(_pc_prefix_expr    "${CMAKE_INSTALL_PREFIX}")
    set(_pc_libdir_expr    "${CMAKE_INSTALL_LIBDIR}")
    set(_pc_includedir_expr "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    string(REPLACE "/" ";" _pc_libdir_parts "${CMAKE_INSTALL_LIBDIR}")
    list(LENGTH _pc_libdir_parts _pc_libdir_depth)
    set(_pc_up "..")
    foreach(_i RANGE 1 ${_pc_libdir_depth})
        string(APPEND _pc_up "/..")
    endforeach()
    set(_pc_prefix_expr     "\${pcfiledir}/${_pc_up}")
    set(_pc_libdir_expr     "${_pc_prefix_expr}/${CMAKE_INSTALL_LIBDIR}")
    set(_pc_includedir_expr "${_pc_prefix_expr}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sqlconduit.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/sqlconduit.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sqlconduit.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

foreach(_component MySQL Postgres ODBC Oracle)
    list(FIND SQLCONDUIT_BUILT_COMPONENTS "${_component}" _component_index)
    if(NOT _component_index EQUAL -1)
        string(TOLOWER "${_component}" SQLCONDUIT_PC_DRIVER)
        set(SQLCONDUIT_PC_DRIVER_DISPLAY "${_component}")
        string(TOUPPER "${_component}" _component_upper)
        get_property(SQLCONDUIT_PC_DRIVER_LIBS_PRIVATE GLOBAL PROPERTY
                     "SQLCONDUIT_${_component_upper}_PC_LIBS_PRIVATE")
        list(REMOVE_DUPLICATES SQLCONDUIT_PC_DRIVER_LIBS_PRIVATE)
        string(REPLACE ";" " " SQLCONDUIT_PC_DRIVER_LIBS_PRIVATE
               "${SQLCONDUIT_PC_DRIVER_LIBS_PRIVATE}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sqlconduit-driver.pc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/sqlconduit-${SQLCONDUIT_PC_DRIVER}.pc" @ONLY)
        install(FILES
                "${CMAKE_CURRENT_BINARY_DIR}/sqlconduit-${SQLCONDUIT_PC_DRIVER}.pc"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
    endif()
endforeach()
