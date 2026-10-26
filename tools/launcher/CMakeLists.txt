# PaulaLauncher.exe - the player-side launcher (see README.md). Built by the root CMakeLists.txt;
# like the servers it lands in the project root. Ship it to players with launcher.ini.
add_executable(PaulaLauncher launcher.cpp)

target_compile_definitions(PaulaLauncher PRIVATE PAULA_LAUNCHER_VERSION="${PROJECT_VERSION}")

# Static CRT: the exe needs only KERNEL32, so players install no Visual C++ redistributable.
set_target_properties(PaulaLauncher PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The client is large-address-aware: with this flag the 32-bit launcher's memory scan also
# reaches client memory above 2 GB.
if(MSVC)
    target_link_options(PaulaLauncher PRIVATE /LARGEADDRESSAWARE)
endif()
