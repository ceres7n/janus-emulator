#pragma once
#include <asio.hpp>
#include <functional>
#include <string>
#include "config.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The start-up and run plumbing the four server mains share
// ─────────────────────────────────────────────────────────────────────────────
namespace paula {

// The conf path: argv[1] made absolute against the directory the server was started from, or `def`
// (relative: resolved in the exe's folder once chdir_to_exe_folder() ran).
std::string conf_path_arg(int argc, char* argv[], const char* def);

// Makes the folder of the running exe the working directory, so the relative conf/, db/, data/ and
// tools/ paths (and the <prefix>_live.log files) resolve the same however the server was started.
// Call it first in main, before anything is logged: the live log opens in the working directory.
bool chdir_to_exe_folder();

// Conf key log_level: debug | info | warn | error (default info). An unknown value keeps info.
void apply_log_level(const Config& cfg);

// Conf key bind_ip: the IPv4 address the server listens on (0.0.0.0 = every interface; an empty or
// missing value = def). False, with a FATAL line, when the value is not an IPv4 address.
bool bind_address(const Config& cfg, const char* def, asio::ip::address& out);

// Opens, binds and listens an acceptor on ep. SO_REUSEADDR stays off: on Windows it would let a
// second copy of the server bind the same port without an error. False, with a FATAL line, on failure.
bool open_acceptor(asio::ip::tcp::acceptor& acc, const asio::ip::tcp::endpoint& ep, const char* what);

// Ctrl+C, Ctrl+Break, closing the console window (also taskkill without /F) and system shutdown run
// request_stop once, on the console-control thread (SIGINT / SIGTERM where there is no console API).
// On a close or shutdown Windows ends the process as soon as the handler returns (and after about
// 5 s in any case), so the handler then waits up to 4.5 s for notify_stopped().
void install_stop_handler(std::function<void()> request_stop);

// The stop has finished (the GameServer: its players are saved). Also call it when main returns.
void notify_stopped();

// asio's pattern: an exception thrown by a handler leaves run() — log it and run again, so one bad
// timer / post handler does not end the server (the packet path catches its own,
// Session::dispatch_packet). The main thread and every pool thread run this.
void run_loop(asio::io_context& ioc);

} // namespace paula
