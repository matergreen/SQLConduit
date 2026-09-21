cmake_minimum_required(VERSION 3.16)
project(sqlconduit VERSION 0.6.0 LANGUAGES CXX)

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
option(SQLCONDUIT_BUILD_INTEGRATION_TESTS
       "Build integration tests that require live database services"          OFF)
option(SQLCONDUIT_ENABLE_ASYNC_CORO "Build the C++20 coroutine layer (task.h)"     OFF)

set(_nl_inc_dir "")
set(_nl_install_src "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    set(_nl_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    set(_nl_install_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(STATUS "sqlconduit: using vendored nlohmann/json (will install into include/nlohmann/)")
else()
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND AND TARGET nlohmann_json::nlohmann_json)
        get_target_property(_nl_inc_dir nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
        find_file(_nl_install_src NAMES nlohmann/json.hpp HINTS ${_nl_inc_dir}
                  NO_DEFAULT_PATH REQUIRED)
        message(STATUS "sqlconduit: using system nlohmann_json (will install a copy)")
    else()
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3)
        FetchContent_MakeAvailable(nlohmann_json)
        set(_nl_inc_dir "${nlohmann_json_SOURCE_DIR}/single_include")
        set(_nl_install_src "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann/json.hpp")
        message(STATUS "sqlconduit: fetched nlohmann_json via FetchContent (will install a copy)")
    endif()
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${_nl_inc_dir}>
    $<INSTALL_INTERFACE:include>)
install(FILES ${_nl_install_src} DESTINATION include/nlohmann)
install(TARGETS nlohmann_json EXPORT sqlconduitTargets)

set(SQLCONDUIT_SOURCES
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
    src/async/async_engine.cpp
    src/core/idatabase_connection.cpp
    src/core/connection_pool.cpp
    src/core/heartbeat_manager.cpp
    src/core/database_manager.cpp
    src/core/sql_auditor.cpp
    src/core/query_cache.cpp
    src/core/write_buffer.cpp
    src/core/stats_reporter.cpp
    src/core/interceptor.cpp
    src/driver/driver_registry.cpp
    src/driver/driver_factory.cpp
    src/driver/mysql_driver.cpp
    src/driver/postgres_driver.cpp
    src/driver/odbc_driver.cpp
    src/driver/oracle_driver.cpp
    src/sqlconduit.cpp
    src/exporters/prometheus.cpp
)

if(SQLCONDUIT_ENABLE_ASYNC_CORO)
    add_library(sqlconduit_coro OBJECT src/async/task.cpp)
    target_include_directories(sqlconduit_coro PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    set_target_properties(sqlconduit_coro PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(sqlconduit_coro PRIVATE SQLCONDUIT_ENABLE_ASYNC_CORO=1)
    set(SQLCONDUIT_CORO_OBJ "$<TARGET_OBJECTS:sqlconduit_coro>")
else()
    set(SQLCONDUIT_CORO_OBJ "")
endif()

add_library(sqlconduit STATIC ${SQLCONDUIT_SOURCES} ${SQLCONDUIT_CORO_OBJ})
target_include_directories(sqlconduit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(sqlconduit PUBLIC nlohmann_json)
if(SQLCONDUIT_ENABLE_ASYNC_CORO)
    target_compile_definitions(sqlconduit PUBLIC SQLCONDUIT_ENABLE_ASYNC_CORO=1)
endif()

if(SQLCONDUIT_ENABLE_MYSQL)
    target_compile_definitions(sqlconduit PRIVATE SQLCONDUIT_ENABLE_MYSQL)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
    find_library(MYSQL_LIBRARY  NAMES mysqlclient libmysql)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_MYSQL=ON but libmysqlclient not found. "
            "Run: sudo apt install -y default-libmysqlclient-dev")
    endif()
    target_include_directories(sqlconduit PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(sqlconduit PRIVATE ${MYSQL_LIBRARY})
endif()

if(SQLCONDUIT_ENABLE_POSTGRES)
    target_compile_definitions(sqlconduit PRIVATE SQLCONDUIT_ENABLE_POSTGRES)
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
        message(STATUS "sqlconduit: libpqxx >= 8 requires C++20; raising only the sqlconduit target")
        set_target_properties(sqlconduit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()

    target_include_directories(sqlconduit PRIVATE ${PQXX_INCLUDE_DIR} ${PQ_INCLUDE_DIR})
    target_link_libraries(sqlconduit PRIVATE ${PQXX_LIBRARY} ${PQ_LIBRARY})
endif()

if(SQLCONDUIT_ENABLE_ODBC)
    target_compile_definitions(sqlconduit PRIVATE SQLCONDUIT_ENABLE_ODBC)
    find_path(ODBC_INCLUDE_DIR NAMES sql.h)
    find_library(ODBC_LIBRARY  NAMES odbc)
    if(NOT ODBC_INCLUDE_DIR OR NOT ODBC_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_ODBC=ON but unixODBC not found. "
            "Run: sudo apt install -y unixodbc-dev")
    endif()
    target_include_directories(sqlconduit PRIVATE ${ODBC_INCLUDE_DIR})
    target_link_libraries(sqlconduit PRIVATE ${ODBC_LIBRARY})
endif()

if(SQLCONDUIT_ENABLE_ORACLE)
    target_compile_definitions(sqlconduit PRIVATE SQLCONDUIT_ENABLE_ORACLE)
    find_path(OCI_INCLUDE_DIR NAMES oci.h)
    find_library(OCI_LIBRARY  NAMES clntsh oci)
    if(NOT OCI_INCLUDE_DIR OR NOT OCI_LIBRARY)
        message(FATAL_ERROR "SQLCONDUIT_ENABLE_ORACLE=ON but the Oracle client was not found "
            "(oci.h / libclntsh). Install the Oracle Instant Client Basic + SDK packages and "
            "pass -DOCI_INCLUDE_DIR=<dir> -DOCI_LIBRARY=<path>.")
    endif()
    target_include_directories(sqlconduit PRIVATE ${OCI_INCLUDE_DIR})
    target_link_libraries(sqlconduit PRIVATE ${OCI_LIBRARY})
endif()

if(SQLCONDUIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS sqlconduit
        EXPORT sqlconduitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/sqlconduit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(_sqlconduit_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/sqlconduit")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sqlconduitConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfig.cmake"
    INSTALL_DESTINATION "${_sqlconduit_cmake_dir}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sqlconduitConfigVersion.cmake"
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
