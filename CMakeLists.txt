cmake_minimum_required(VERSION 3.20)
project(ProjectPAULA VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/EHsc /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    # The exes name their PDB without the build machine's folder (no C:\Users\... inside):
    # /PDBALTPATH:<name>.pdb. (Spelled out because the VS generator mangles "%_PDB%".)
    add_link_options("/PDBALTPATH:$<IF:$<BOOL:$<TARGET_PROPERTY:OUTPUT_NAME>>,$<TARGET_PROPERTY:OUTPUT_NAME>,$<TARGET_PROPERTY:NAME>>.pdb")
endif()

# The game + vendored libmysql are 32-bit. Configure with: cmake -S . -B build -A Win32
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ProjectPAULA must build 32-bit. Reconfigure with: cmake -A Win32 (libmysql.lib is x86).")
endif()

# rAthena-style layout: server exes land in the project root (next to conf/ tools/ libmysql.dll).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_SOURCE_DIR})

# ── 3rd-party (vendored in 3rdparty/: asio 1.30.2, MySQL Connector/C 6.1.11 x86) ──
# Override with -DPAULA_3RDPARTY=... to build against another copy with the same layout.
set(PAULA_3RDPARTY "${CMAKE_SOURCE_DIR}/3rdparty" CACHE PATH "3rd-party root")
set(ASIO_INCLUDE_DIR  "${PAULA_3RDPARTY}/asio/asio/include")
set(MYSQL_INCLUDE_DIR "${PAULA_3RDPARTY}/mysql/include")
set(MYSQL_LIB         "${PAULA_3RDPARTY}/mysql/lib/libmysql.lib")
set(MYSQL_DLL         "${PAULA_3RDPARTY}/mysql/lib/libmysql.dll")
foreach(_dep "${ASIO_INCLUDE_DIR}/asio.hpp" "${MYSQL_INCLUDE_DIR}/mysql.h" "${MYSQL_LIB}" "${MYSQL_DLL}")
    if(NOT EXISTS "${_dep}")
        message(FATAL_ERROR "Missing ${_dep}: PAULA_3RDPARTY (${PAULA_3RDPARTY}) must hold asio/ and mysql/.")
    endif()
endforeach()

add_subdirectory(src/common)
add_subdirectory(src/authserver)
add_subdirectory(src/loginserver)
add_subdirectory(src/masterserver)
add_subdirectory(src/gameserver)

# The servers load libmysql.dll at start: copy it next to them at build time.
set(PAULA_MYSQL_DLL_OUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/libmysql.dll")
add_custom_command(OUTPUT "${PAULA_MYSQL_DLL_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_DLL}" "${PAULA_MYSQL_DLL_OUT}"
    DEPENDS "${MYSQL_DLL}"
    COMMENT "Copying libmysql.dll next to the servers"
    VERBATIM)
add_custom_target(paula_libmysql ALL DEPENDS "${PAULA_MYSQL_DLL_OUT}")
foreach(_srv authserver loginserver masterserver gameserver)
    add_dependencies(${_srv} paula_libmysql)
endforeach()

# Player-side launcher (PaulaLauncher.exe): starts Trickster.bin and points it at the server.
add_subdirectory(tools/launcher)
# Player-side launcher with a window (JanusLauncher.exe): host names, suspended start, no console.
add_subdirectory(tools/janus-launcher)
