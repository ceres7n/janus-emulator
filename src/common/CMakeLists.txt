add_library(paula_common STATIC
    packet.cpp
    config.cpp
    database.cpp
    logger.cpp
    server_runtime.cpp
    session.cpp
    paula_crypt.cpp
    client_text.cpp
    interserver/inter_link.cpp
    store/char_store.cpp
    store/item_store.cpp
    store/post_store.cpp
)

target_include_directories(paula_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ASIO_INCLUDE_DIR}
        ${MYSQL_INCLUDE_DIR}
)

target_compile_definitions(paula_common
    PUBLIC
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        _WIN32_WINNT=0x0601
)

target_link_libraries(paula_common PUBLIC ${MYSQL_LIB})

if(WIN32)
    target_link_libraries(paula_common PUBLIC ws2_32 wsock32)
endif()
