# Re-discovers the driver client libraries in the consumer's own environment.
#
# The driver libraries are deliberately kept out of the installed export so that
# the package stays relocatable: the export points at names, this module resolves
# them to whatever the consumer's machine actually has.
#
# Input:  SQLCONDUIT_DRIVER_DEP_NAMES - list of MYSQL/POSTGRES/ODBC/ORACLE
# Output: <out_var> - list of resolved library paths
#
# Each search uses its own cache variable: find_library() stores results in the
# cache, so reusing one variable name across searches silently skips the later ones.

function(sqlconduit_resolve_driver_deps out_var)
    set(_sqlconduit_ld_dirs "")
    if(DEFINED ENV{LD_LIBRARY_PATH})
        string(REPLACE ":" ";" _sqlconduit_ld_dirs "$ENV{LD_LIBRARY_PATH}")
    endif()

    set(_sqlconduit_libs "")
    foreach(_sqlconduit_drv IN LISTS SQLCONDUIT_DRIVER_DEP_NAMES)
        set(_sqlconduit_found "")

        if(_sqlconduit_drv STREQUAL "MYSQL")
            set(_sqlconduit_desc "the MySQL client library (libmysqlclient)")
            find_library(_sqlconduit_mysql_lib NAMES mysqlclient libmysql)
            if(_sqlconduit_mysql_lib)
                list(APPEND _sqlconduit_found "${_sqlconduit_mysql_lib}")
            endif()

        elseif(_sqlconduit_drv STREQUAL "POSTGRES")
            set(_sqlconduit_desc "the PostgreSQL client libraries (libpqxx/libpq)")
            if(UNIX AND NOT APPLE)
                find_library(_sqlconduit_pqxx_lib NAMES libpqxx.a pqxx)
            else()
                find_library(_sqlconduit_pqxx_lib NAMES pqxx)
            endif()
            find_library(_sqlconduit_pq_lib NAMES pq)
            if(_sqlconduit_pqxx_lib AND _sqlconduit_pq_lib)
                list(APPEND _sqlconduit_found "${_sqlconduit_pqxx_lib}" "${_sqlconduit_pq_lib}")
            endif()

        elseif(_sqlconduit_drv STREQUAL "ODBC")
            set(_sqlconduit_desc "unixODBC (libodbc)")
            find_library(_sqlconduit_odbc_lib NAMES odbc)
            if(_sqlconduit_odbc_lib)
                list(APPEND _sqlconduit_found "${_sqlconduit_odbc_lib}")
            endif()

        elseif(_sqlconduit_drv STREQUAL "ORACLE")
            set(_sqlconduit_desc "the Oracle client library (libclntsh)")
            find_library(_sqlconduit_oci_lib NAMES clntsh oci
                         HINTS "${SQLCONDUIT_OCI_LIBRARY_DIR}" "$ENV{ORACLE_HOME}/lib"
                               ${_sqlconduit_ld_dirs})
            if(_sqlconduit_oci_lib)
                list(APPEND _sqlconduit_found "${_sqlconduit_oci_lib}")
            endif()

        else()
            message(FATAL_ERROR "sqlconduit: unknown driver dependency '${_sqlconduit_drv}'")
        endif()

        if(NOT _sqlconduit_found)
            message(FATAL_ERROR
                "sqlconduit was built with the ${_sqlconduit_drv} driver but "
                "${_sqlconduit_desc} was not found in this environment. Install the client "
                "development package, or point CMAKE_PREFIX_PATH / CMAKE_LIBRARY_PATH at it. "
                "For Oracle, set SQLCONDUIT_OCI_LIBRARY_DIR, ORACLE_HOME or LD_LIBRARY_PATH.")
        endif()
        list(APPEND _sqlconduit_libs ${_sqlconduit_found})
    endforeach()

    set(${out_var} "${_sqlconduit_libs}" PARENT_SCOPE)
endfunction()
