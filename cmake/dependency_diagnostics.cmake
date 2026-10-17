function(sqlconduit_missing_dependency component details)
    if(WIN32)
        set(_hint "Use the vcpkg CMake toolchain and install libmysql/libpqxx as needed. Windows ODBC uses the SDK odbc32 library; install a runtime ODBC driver separately.")
    elseif(APPLE)
        set(_hint "Use brew install mysql-client libpq libpqxx unixodbc; pass keg-only prefixes through CMAKE_PREFIX_PATH (brew --prefix <formula>).")
    else()
        set(_hint "On Ubuntu 24.04+: apt install default-libmysqlclient-dev libpqxx-dev libpq-dev unixodbc-dev. Other distributions need equivalent development packages.")
    endif()
    message(FATAL_ERROR "SQLConduit ${component} dependency missing: ${details}\n${_hint}\n"
        "For a custom SDK, set CMAKE_PREFIX_PATH and the component's *_INCLUDE_DIR / *_LIBRARY cache variables.\n"
        "Oracle requires separately installed Instant Client Basic + SDK and OCI_INCLUDE_DIR / OCI_LIBRARY.\n"
        "Disable an unused component with SQLCONDUIT_ENABLE_<DRIVER>=OFF; Core needs no database SDK.")
endfunction()
