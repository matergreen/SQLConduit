cmake_minimum_required(VERSION 3.16)
project(dbmw VERSION 0.5.1 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(DBMW_ENABLE_MYSQL    "Build MySQL driver (needs libmysqlclient)"      OFF)
option(DBMW_ENABLE_POSTGRES "Build PostgreSQL driver (needs libpqxx + libpq)" OFF)
option(DBMW_ENABLE_ODBC     "Build ODBC driver for SQL Server/Oracle"         OFF)
option(DBMW_BUILD_TESTS     "Build core behaviour tests (no external deps)"   OFF)
option(DBMW_BUILD_INTEGRATION_TESTS
       "Build integration tests that require live database services"          OFF)
option(DBMW_ENABLE_ASYNC_CORO "Build the C++20 coroutine layer (task.h)"     OFF)

set(_nl_inc_dir "")
set(_nl_install_src "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    set(_nl_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    set(_nl_install_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(STATUS "dbmw: using vendored nlohmann/json (will install into include/nlohmann/)")
else()
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND AND TARGET nlohmann_json::nlohmann_json)
        get_target_property(_nl_inc_dir nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
        find_file(_nl_install_src NAMES nlohmann/json.hpp HINTS ${_nl_inc_dir}
                  NO_DEFAULT_PATH REQUIRED)
        message(STATUS "dbmw: using system nlohmann_json (will install a copy)")
    else()
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3)
        FetchContent_MakeAvailable(nlohmann_json)
        set(_nl_inc_dir "${nlohmann_json_SOURCE_DIR}/single_include")
        set(_nl_install_src "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann/json.hpp")
        message(STATUS "dbmw: fetched nlohmann_json via FetchContent (will install a copy)")
    endif()
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${_nl_inc_dir}>
    $<INSTALL_INTERFACE:include>)
install(FILES ${_nl_install_src} DESTINATION include/nlohmann)
install(TARGETS nlohmann_json EXPORT dbmwTargets)

set(DBMW_SOURCES
    src/common/types.cpp
    src/common/pg_types.cpp
    src/common/context.cpp
    src/common/observer.cpp
    src/common/sql_analyze.cpp
    src/config/datasource_config.cpp
    src/config/config_loader.cpp
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
    src/dbmw.cpp
    src/exporters/prometheus.cpp
)

if(DBMW_ENABLE_ASYNC_CORO)
    add_library(dbmw_coro OBJECT src/async/task.cpp)
    target_include_directories(dbmw_coro PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    set_target_properties(dbmw_coro PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(dbmw_coro PRIVATE DBMW_ENABLE_ASYNC_CORO=1)
    set(DBMW_CORO_OBJ "$<TARGET_OBJECTS:dbmw_coro>")
else()
    set(DBMW_CORO_OBJ "")
endif()

add_library(dbmw STATIC ${DBMW_SOURCES} ${DBMW_CORO_OBJ})
target_include_directories(dbmw PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(dbmw PUBLIC nlohmann_json)
if(DBMW_ENABLE_ASYNC_CORO)
    target_compile_definitions(dbmw PUBLIC DBMW_ENABLE_ASYNC_CORO=1)
endif()

if(DBMW_ENABLE_MYSQL)
    target_compile_definitions(dbmw PRIVATE DBMW_ENABLE_MYSQL)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
    find_library(MYSQL_LIBRARY  NAMES mysqlclient libmysql)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "DBMW_ENABLE_MYSQL=ON but libmysqlclient not found. "
            "Run: sudo apt install -y default-libmysqlclient-dev")
    endif()
    target_include_directories(dbmw PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(dbmw PRIVATE ${MYSQL_LIBRARY})
endif()

if(DBMW_ENABLE_POSTGRES)
    target_compile_definitions(dbmw PRIVATE DBMW_ENABLE_POSTGRES)
    find_path(PQXX_INCLUDE_DIR NAMES pqxx/pqxx)
    if(UNIX AND NOT APPLE)
        # Linux 下优先静态链 libpqxx.a：pqxx 的 pqxx::internal::type_name 是 C++17
        # inline 模板变量，若按共享库链接，可执行文件与 libpqxx.so 会各持一份实例、
        # 各注册一次析构器，进程退出时双重释放（ASan/glibc 报 double free）。静态链入
        # 后该 inline 静态合并为单一实例、单一析构。无 .a 时回退共享库。
        find_library(PQXX_LIBRARY NAMES libpqxx.a pqxx)
    else()
        find_library(PQXX_LIBRARY NAMES pqxx)
    endif()
    find_path(PQ_INCLUDE_DIR    NAMES postgresql/libpq-fe.h libpq-fe.h)
    find_library(PQ_LIBRARY     NAMES pq)
    if(NOT PQXX_INCLUDE_DIR OR NOT PQXX_LIBRARY OR NOT PQ_LIBRARY)
        message(FATAL_ERROR "DBMW_ENABLE_POSTGRES=ON but libpqxx/libpq not found. "
            "Run: sudo apt install -y libpqxx-dev libpq-dev")
    endif()

    file(STRINGS "${PQXX_INCLUDE_DIR}/pqxx/version.hxx" _dbmw_pqxx_major_line
         REGEX "^#[ ]*define[ ]+PQXX_VERSION_MAJOR[ ]+[0-9]+")
    string(REGEX MATCH "([0-9]+)" _dbmw_pqxx_major "${_dbmw_pqxx_major_line}")
    if(NOT _dbmw_pqxx_major)
        message(WARNING "dbmw: 无法从 ${PQXX_INCLUDE_DIR}/pqxx/version.hxx 中解析 "
                        "PQXX_VERSION_MAJOR，按 C++17 构建。")
        set(_dbmw_pqxx_major 7)
    endif()
    if(_dbmw_pqxx_major LESS 7)
        message(FATAL_ERROR "dbmw: 需要 libpqxx >= 7，当前主版本为 ${_dbmw_pqxx_major}。"
            " 6.x 缺少 pqxx::zview / pqxx::params，请改用 Ubuntu 24.04+ 或 Homebrew 的"
            " libpqxx，或关闭 -DDBMW_ENABLE_POSTGRES=OFF。")
    endif()
    message(STATUS "dbmw: libpqxx 主版本 ${_dbmw_pqxx_major}")
    if(_dbmw_pqxx_major GREATER_EQUAL 8)
        message(STATUS "dbmw: libpqxx >= 8 要求 C++20，dbmw 目标单独提标至 C++20")
        set_target_properties(dbmw PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()

    target_include_directories(dbmw PRIVATE ${PQXX_INCLUDE_DIR} ${PQ_INCLUDE_DIR})
    target_link_libraries(dbmw PRIVATE ${PQXX_LIBRARY} ${PQ_LIBRARY})
endif()

if(DBMW_ENABLE_ODBC)
    target_compile_definitions(dbmw PRIVATE DBMW_ENABLE_ODBC)
    find_path(ODBC_INCLUDE_DIR NAMES sql.h)
    find_library(ODBC_LIBRARY  NAMES odbc)
    if(NOT ODBC_INCLUDE_DIR OR NOT ODBC_LIBRARY)
        message(FATAL_ERROR "DBMW_ENABLE_ODBC=ON but unixODBC not found. "
            "Run: sudo apt install -y unixodbc-dev")
    endif()
    target_include_directories(dbmw PRIVATE ${ODBC_INCLUDE_DIR})
    target_link_libraries(dbmw PRIVATE ${ODBC_LIBRARY})
endif()

if(DBMW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS dbmw
        EXPORT dbmwTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/dbmw DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(_dbmw_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/dbmw")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dbmwConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dbmwConfig.cmake"
    INSTALL_DESTINATION "${_dbmw_cmake_dir}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/dbmwConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/dbmwConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/dbmwConfigVersion.cmake"
    DESTINATION "${_dbmw_cmake_dir}")
install(EXPORT dbmwTargets
        NAMESPACE dbmw::
        DESTINATION "${_dbmw_cmake_dir}")

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
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dbmw.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/dbmw.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/dbmw.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
