add_executable(loginserver
    main.cpp
    login_server.cpp
    client_session.cpp
)

target_include_directories(loginserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(loginserver PRIVATE paula_common ${MYSQL_LIB})
if(WIN32)
    target_link_libraries(loginserver PRIVATE ws2_32 wsock32)
endif()

set_target_properties(loginserver PROPERTIES OUTPUT_NAME "LoginServer")
