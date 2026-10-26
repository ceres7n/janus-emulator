add_executable(gameserver
    main.cpp
    game_server.cpp
    game_session.cpp
    atcommand.cpp
    skill.cpp
    skill_magic.cpp
    skill_power.cpp
    skill_sense.cpp
    skill_charm.cpp
    skill_bard.cpp
    skill_dragon.cpp
    skill_thief.cpp
    skill_cat.cpp
    skill_card.cpp
    skill_gm.cpp
    skill_bear.cpp
    party.cpp
    party_quest.cpp
    chat_server.cpp
    friend_server.cpp
    system_session.cpp
    progress.cpp
    drop.cpp
    inventory.cpp
    item_use.cpp
    equip.cpp
    pet.cpp
    trade.cpp
    postbox.cpp
    megaphone.cpp
    memory_port.cpp
    gun.cpp
    es.cpp
    regen.cpp
    npc_talk.cpp
    monster_ai.cpp
    shop.cpp
    drill.cpp
    quest.cpp
    monster_quest.cpp
    exchange_shop.cpp
    warehouse.cpp
    bank.cpp
    compound.cpp
    refine.cpp
    skill_shop.cpp
    world/entity_manager.cpp
)

target_include_directories(gameserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gameserver PRIVATE paula_common ${MYSQL_LIB})
if(WIN32)
    target_link_libraries(gameserver PRIVATE ws2_32 wsock32 dbghelp)
endif()
if(MSVC)
    # A PDB for the Release build too (it resolves the crash dumps of main.cpp); /OPT:REF,ICF keep the release link.
    target_compile_options(gameserver PRIVATE $<$<CONFIG:Release>:/Zi /FS>)
    target_link_options(gameserver PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

set_target_properties(gameserver PROPERTIES OUTPUT_NAME "GameServer")
