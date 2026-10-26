#include <string>
#include <thread>
#include <vector>
#include <asio.hpp>
#include "auth_server.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "server_runtime.hpp"
#include "version.hpp"

int main(int argc, char* argv[])
{
    const std::string conf_path = paula::conf_path_arg(argc, argv, "conf/auth_server.conf");
    const bool in_exe_folder = paula::chdir_to_exe_folder();
    Logger::instance().set_prefix("auth");
    LOG_INFO("ProjectPAULA AuthServer v%s", PAULA_VERSION);
    if (!in_exe_folder)
        LOG_WARN("Could not change to the exe's folder; relative paths use the current directory.");

    Config cfg;
    if (!cfg.load(conf_path))
        LOG_WARN("Config not found (%s); using defaults.", conf_path.c_str());
    paula::apply_log_level(cfg);

    int threads = cfg.get_int("io_threads", 2);
    asio::io_context ioc(threads);

    cAuthServer server(ioc);
    if (!server.init(cfg)) {
        LOG_FATAL("AuthServer init failed");
        return 1;
    }
    paula::install_stop_handler([&ioc]() {
        LOG_INFO("AuthServer shutting down...");
        ioc.stop();
        paula::notify_stopped();
    });
    server.run();

    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i)
        pool.emplace_back([&ioc]() { paula::run_loop(ioc); });

    LOG_INFO("AuthServer started (%d threads)", threads);
    paula::run_loop(ioc);

    for (auto& t : pool) t.join();
    LOG_INFO("AuthServer stopped.");
    paula::notify_stopped();
    return 0;
}
