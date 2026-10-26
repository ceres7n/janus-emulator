#include "server_runtime.hpp"
#include "logger.hpp"
#include <atomic>
#include <csignal>
#include <filesystem>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace paula {

std::string conf_path_arg(int argc, char* argv[], const char* def)
{
    if (argc < 2 || !argv[1] || !argv[1][0]) return def;
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::absolute(argv[1], ec);
    return ec ? std::string(argv[1]) : p.string();
}

bool chdir_to_exe_folder()
{
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return false;
    std::error_code ec;
    std::filesystem::current_path(std::filesystem::path(std::wstring(buf.data(), n)).parent_path(), ec);
    return !ec;
#else
    return true;
#endif
}

void apply_log_level(const Config& cfg)
{
    const std::string v = cfg.get_str("log_level", "info");
    LogLevel lvl = LogLevel::Info;
    if (!Logger::parse_level(v, lvl))
        LOG_WARN("log_level '%s' unknown (debug | info | warn | error); using info", v.c_str());
    Logger::instance().set_level(lvl);
}

bool bind_address(const Config& cfg, const char* def, asio::ip::address& out)
{
    std::string s = cfg.get_str("bind_ip", def);
    if (s.empty()) s = def;
    asio::error_code ec;
    const asio::ip::address_v4 a = asio::ip::make_address_v4(s, ec);
    if (ec) {
        LOG_FATAL("bind_ip '%s' is not an IPv4 address (0.0.0.0 = every interface)", s.c_str());
        return false;
    }
    out = a;
    return true;
}

bool open_acceptor(asio::ip::tcp::acceptor& acc, const asio::ip::tcp::endpoint& ep, const char* what)
{
    const std::string where = ep.address().to_string() + ":" + std::to_string(ep.port());
    asio::error_code ec;
    acc.open(ep.protocol(), ec);
    if (ec) { LOG_FATAL("%s acceptor open: %s", what, ec.message().c_str()); return false; }
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(false), ec);
    acc.bind(ep, ec);
    if (ec) {
        LOG_FATAL("%s bind %s: %s (is another copy of this server running?)", what, where.c_str(),
                  ec.message().c_str());
        return false;
    }
    acc.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { LOG_FATAL("%s listen %s: %s", what, where.c_str(), ec.message().c_str()); return false; }
    return true;
}

namespace {

// Never destroyed: the console-control thread may still run while main returns and the statics go.
std::function<void()>* g_request_stop = nullptr;
std::atomic<bool>      g_stop_requested{ false };
#ifdef _WIN32
HANDLE                 g_stopped_event = nullptr;   // manual reset, set by notify_stopped()
#endif

void request_stop_once()
{
    if (g_stop_requested.exchange(true)) return;    // a second Ctrl+C while stopping changes nothing
    if (g_request_stop && *g_request_stop) {
        try { (*g_request_stop)(); }
        catch (const std::exception& e) { LOG_ERROR("stop request failed: %s", e.what()); }
        catch (...)                     { LOG_ERROR("stop request failed"); }
    }
}

#ifdef _WIN32
BOOL WINAPI console_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        request_stop_once();
        return TRUE;                    // handled: main returns once the stop has run
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        request_stop_once();
        // Windows ends the process as soon as this returns: let the stop (the GameServer's save) finish.
        if (g_stopped_event) WaitForSingleObject(g_stopped_event, 4500);
        return TRUE;
    default:
        return FALSE;                   // CTRL_LOGOFF_EVENT (another user logging off): not a stop
    }
}
#else
void on_signal(int) { request_stop_once(); }
#endif

} // namespace

void install_stop_handler(std::function<void()> request_stop)
{
    g_request_stop = new std::function<void()>(std::move(request_stop));
#ifdef _WIN32
    if (!g_stopped_event) g_stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!SetConsoleCtrlHandler(console_ctrl, TRUE))
        LOG_WARN("SetConsoleCtrlHandler failed (%lu): Ctrl+C / closing the console will not stop cleanly",
                 GetLastError());
#else
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
#endif
}

void notify_stopped()
{
    g_stop_requested = true;            // a later Ctrl+C / close has nothing left to stop
#ifdef _WIN32
    if (g_stopped_event) SetEvent(g_stopped_event);
#endif
}

void run_loop(asio::io_context& ioc)
{
    for (;;) {
        try { ioc.run(); return; }
        catch (const std::exception& e) { LOG_ERROR("io_context: a handler threw: %s", e.what()); }
        catch (...)                     { LOG_ERROR("io_context: a handler threw an unknown exception"); }
    }
}

} // namespace paula
