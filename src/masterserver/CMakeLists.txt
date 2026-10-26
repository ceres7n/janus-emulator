add_executable(masterserver
    main.cpp
    master_server.cpp
)

target_include_directories(masterserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(masterserver PRIVATE paula_common ${MYSQL_LIB})
if(WIN32)
    target_link_libraries(masterserver PRIVATE ws2_32 wsock32)
endif()

set_target_properties(masterserver PROPERTIES OUTPUT_NAME "MasterServer")
