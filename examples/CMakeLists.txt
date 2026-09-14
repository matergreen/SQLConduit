add_executable(dbmw_example_basic basic_usage.cpp)
target_link_libraries(dbmw_example_basic PRIVATE dbmw)

add_executable(dbmw_example_async async_example.cpp)
target_link_libraries(dbmw_example_async PRIVATE dbmw)
if(DBMW_ENABLE_ASYNC_CORO)
    set_target_properties(dbmw_example_async PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()
