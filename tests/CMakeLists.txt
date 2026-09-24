add_executable(sqlconduit_core_test sqlconduit_core_test.cpp)
target_link_libraries(sqlconduit_core_test PRIVATE sqlconduit_core)
target_compile_definitions(sqlconduit_core_test PRIVATE SQLCONDUIT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(sqlconduit_mapping_test sqlconduit_mapping_test.cpp)
target_link_libraries(sqlconduit_mapping_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_pg_types_test sqlconduit_pg_types_test.cpp)
target_link_libraries(sqlconduit_pg_types_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_oracle_types_test sqlconduit_oracle_types_test.cpp)
target_link_libraries(sqlconduit_oracle_types_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_util_test sqlconduit_util_test.cpp)
target_link_libraries(sqlconduit_util_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_context_test sqlconduit_context_test.cpp)
target_link_libraries(sqlconduit_context_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_interceptor_test sqlconduit_interceptor_test.cpp)
target_link_libraries(sqlconduit_interceptor_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_observability_test sqlconduit_observability_test.cpp)
target_link_libraries(sqlconduit_observability_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_exporters_test sqlconduit_exporters_test.cpp)
target_link_libraries(sqlconduit_exporters_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_dynamic_test sqlconduit_dynamic_test.cpp)
target_link_libraries(sqlconduit_dynamic_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_idempotency_test sqlconduit_idempotency_test.cpp)
target_link_libraries(sqlconduit_idempotency_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_shadow_test sqlconduit_shadow_test.cpp)
target_link_libraries(sqlconduit_shadow_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_redaction_test sqlconduit_redaction_test.cpp)
target_link_libraries(sqlconduit_redaction_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_raw_session_test sqlconduit_raw_session_test.cpp)
target_link_libraries(sqlconduit_raw_session_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_observer_event_test sqlconduit_observer_event_test.cpp)
target_link_libraries(sqlconduit_observer_event_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_limiter_plugin_test sqlconduit_limiter_plugin_test.cpp)
target_link_libraries(sqlconduit_limiter_plugin_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_client_test sqlconduit_client_test.cpp)
target_link_libraries(sqlconduit_client_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_public_api_contract_test
               sqlconduit_public_api_contract_test.cpp)
target_link_libraries(sqlconduit_public_api_contract_test PRIVATE sqlconduit_core)

add_executable(sqlconduit_config_schema_test sqlconduit_config_schema_test.cpp)
target_link_libraries(sqlconduit_config_schema_test PRIVATE sqlconduit_core)
target_include_directories(sqlconduit_config_schema_test PRIVATE ${_nl_inc_dir})
target_compile_definitions(sqlconduit_config_schema_test
                           PRIVATE SQLCONDUIT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Every installed header must compile as the first and only project include. This
# catches accidental reliance on transitive includes before an API release.
set(SQLCONDUIT_PUBLIC_HEADERS ${SQLCONDUIT_FROZEN_PUBLIC_HEADERS})
set(SQLCONDUIT_PUBLIC_HEADER_SOURCES "")
foreach(_header IN LISTS SQLCONDUIT_PUBLIC_HEADERS)
    string(MAKE_C_IDENTIFIER "${_header}" _header_id)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/public_header_${_header_id}.cpp")
    file(GENERATE OUTPUT "${_source}" CONTENT "#include \"${_header}\"\n")
    list(APPEND SQLCONDUIT_PUBLIC_HEADER_SOURCES "${_source}")
endforeach()
add_library(sqlconduit_public_headers_compile OBJECT
            ${SQLCONDUIT_PUBLIC_HEADER_SOURCES})
target_link_libraries(sqlconduit_public_headers_compile PRIVATE sqlconduit_core)
set_target_properties(sqlconduit_public_headers_compile PROPERTIES
                      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(SQLCONDUIT_BUILD_INTEGRATION_TESTS AND SQLCONDUIT_ENABLE_POSTGRES)
    add_executable(sqlconduit_postgres_integration_test sqlconduit_postgres_integration_test.cpp)
    target_link_libraries(sqlconduit_postgres_integration_test PRIVATE sqlconduit_postgres)
    add_test(NAME sqlconduit_postgres_integration_test COMMAND sqlconduit_postgres_integration_test)
endif()

if(SQLCONDUIT_BUILD_INTEGRATION_TESTS AND SQLCONDUIT_ENABLE_MYSQL)
    add_executable(sqlconduit_mysql_integration_test sqlconduit_mysql_integration_test.cpp)
    target_link_libraries(sqlconduit_mysql_integration_test PRIVATE sqlconduit_mysql)
    add_test(NAME sqlconduit_mysql_integration_test COMMAND sqlconduit_mysql_integration_test)

endif()

if(SQLCONDUIT_BUILD_INTEGRATION_TESTS AND SQLCONDUIT_ENABLE_ODBC)
    add_executable(sqlconduit_odbc_integration_test sqlconduit_odbc_integration_test.cpp)
    target_link_libraries(sqlconduit_odbc_integration_test PRIVATE sqlconduit_odbc)
    add_test(NAME sqlconduit_odbc_integration_test COMMAND sqlconduit_odbc_integration_test)
endif()

if(SQLCONDUIT_BUILD_INTEGRATION_TESTS AND SQLCONDUIT_ENABLE_ORACLE)
    add_executable(sqlconduit_oracle_integration_test sqlconduit_oracle_integration_test.cpp)
    target_link_libraries(sqlconduit_oracle_integration_test PRIVATE sqlconduit_oracle)
    add_test(NAME sqlconduit_oracle_integration_test COMMAND sqlconduit_oracle_integration_test)
endif()

find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(sqlconduit_core_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_mapping_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_util_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_interceptor_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_observability_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_exporters_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_dynamic_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_idempotency_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_shadow_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_redaction_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_raw_session_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_observer_event_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_limiter_plugin_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_client_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_public_api_contract_test PRIVATE Threads::Threads)
    target_link_libraries(sqlconduit_config_schema_test PRIVATE Threads::Threads)
    if(TARGET sqlconduit_postgres_integration_test)
        target_link_libraries(sqlconduit_postgres_integration_test PRIVATE Threads::Threads)
    endif()
    if(TARGET sqlconduit_mysql_integration_test)
        target_link_libraries(sqlconduit_mysql_integration_test PRIVATE Threads::Threads)
    endif()
    if(TARGET sqlconduit_odbc_integration_test)
        target_link_libraries(sqlconduit_odbc_integration_test PRIVATE Threads::Threads)
    endif()
    if(TARGET sqlconduit_oracle_integration_test)
        target_link_libraries(sqlconduit_oracle_integration_test PRIVATE Threads::Threads)
    endif()
endif()

add_test(NAME sqlconduit_core_test COMMAND sqlconduit_core_test)
add_test(NAME sqlconduit_mapping_test COMMAND sqlconduit_mapping_test)
add_test(NAME sqlconduit_pg_types_test COMMAND sqlconduit_pg_types_test)
add_test(NAME sqlconduit_oracle_types_test COMMAND sqlconduit_oracle_types_test)
add_test(NAME sqlconduit_util_test COMMAND sqlconduit_util_test)
add_test(NAME sqlconduit_context_test COMMAND sqlconduit_context_test)
add_test(NAME sqlconduit_interceptor_test COMMAND sqlconduit_interceptor_test)
add_test(NAME sqlconduit_observability_test COMMAND sqlconduit_observability_test)
add_test(NAME sqlconduit_exporters_test COMMAND sqlconduit_exporters_test)
add_test(NAME sqlconduit_dynamic_test COMMAND sqlconduit_dynamic_test)
add_test(NAME sqlconduit_idempotency_test COMMAND sqlconduit_idempotency_test)
add_test(NAME sqlconduit_shadow_test COMMAND sqlconduit_shadow_test)
add_test(NAME sqlconduit_redaction_test COMMAND sqlconduit_redaction_test)
add_test(NAME sqlconduit_raw_session_test COMMAND sqlconduit_raw_session_test)
add_test(NAME sqlconduit_observer_event_test COMMAND sqlconduit_observer_event_test)
add_test(NAME sqlconduit_limiter_plugin_test COMMAND sqlconduit_limiter_plugin_test)
add_test(NAME sqlconduit_client_test COMMAND sqlconduit_client_test)
add_test(NAME sqlconduit_public_api_contract_test COMMAND sqlconduit_public_api_contract_test)
add_test(NAME sqlconduit_config_schema_test COMMAND sqlconduit_config_schema_test)
