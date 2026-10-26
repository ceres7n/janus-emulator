add_executable(authserver
    main.cpp
    auth_server.cpp
    auth_session.cpp
)

target_include_directories(authserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(authserver PRIVATE paula_common ${MYSQL_LIB})
if(WIN32)
    target_link_libraries(authserver PRIVATE ws2_32 wsock32)
endif()

set_target_properties(authserver PROPERTIES OUTPUT_NAME "AuthServer")
