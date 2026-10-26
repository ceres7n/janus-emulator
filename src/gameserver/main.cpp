#include <string>
#include <thread>
#include <vector>
#include <asio.hpp>
#include "game_server.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "server_runtime.hpp"
#include "version.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <ctime>
#endif

#ifdef _WIN32
// A crash (an access violation, ...): GameServer_crash_<date>_<time>.dmp in the working directory, which main makes
// the exe's folder (MiniDumpWriteDump: the threads' stacks + the data segments) and a log line with the code and
// the address as module + offset (GameServer.pdb resolves both).
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    char name[64];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    std::strftime(name, sizeof(name), "GameServer_crash_%Y%m%d_%H%M%S.dmp", &tmv);
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    const void* addr = er ? er->ExceptionAddress : nullptr;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(addr), &mod);
    char mname[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, mname, MAX_PATH);
    const size_t off = mod ? static_cast<size_t>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)) : 0;
    // The dump first: the log line takes the logger's lock, which the crashed thread may have held.
    HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }
    LOG_FATAL("GameServer CRASH: code 0x%08lX at %p (%s + 0x%zX) - dump %s", er ? er->ExceptionCode : 0ul, addr,
              mname, off, name);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char* argv[])
{
    const std::string conf_path = paula::conf_path_arg(argc, argv, "conf/game_server.conf");
    const bool in_exe_folder = paula::chdir_to_exe_folder();
    Logger::instance().set_prefix("game");
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    LOG_INFO("ProjectPAULA GameServer v%s", PAULA_VERSION);
    if (!in_exe_folder)
        LOG_WARN("Could not change to the exe's folder; relative paths use the current directory.");

    Config cfg;
    if (!cfg.load(conf_path))
        LOG_WARN("Config not found (%s); using defaults.", conf_path.c_str());
    paula::apply_log_level(cfg);

    // One io thread: the world state the sessions share (entity maps, monsters, parties) is not locked
    // against concurrent sessions, and several handlers rely on running one at a time.
    int threads = cfg.get_int("io_threads", 1);
    if (threads != 1) {
        LOG_WARN("io_threads %d ignored: this GameServer version runs on exactly 1 io thread", threads);
        threads = 1;
    }
    asio::io_context ioc(threads);

    cGameServer server(ioc);
    if (!server.init(cfg)) {
        LOG_FATAL("GameServer init failed");
        return 1;
    }
    paula::install_stop_handler([&ioc, &server]() {
        LOG_INFO("GameServer shutting down (saving players)...");
        // The save runs on the io thread, between the sessions' own handlers (not on the console-control
        // thread while they run); the close / shutdown handler waits for notify_stopped().
        asio::post(ioc, [&ioc, &server]() {
            try { server.shutdown_save(); } catch (...) {}  // persist before exit
            paula::notify_stopped();
            ioc.stop();
        });
    });
    server.run();

    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i)
        pool.emplace_back([&ioc]() { paula::run_loop(ioc); });

    LOG_INFO("GameServer started (%d threads)", threads);
    paula::run_loop(ioc);

    for (auto& t : pool) t.join();
    LOG_INFO("GameServer stopped.");
    paula::notify_stopped();
    return 0;
}
