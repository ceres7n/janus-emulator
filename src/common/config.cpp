#include "config.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

bool Config::load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Config: cannot open '%s'", path.c_str());
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        ++lineno;
        // Strip comments (// and #)
        auto cpos = line.find("//");
        if (cpos != std::string::npos) line.erase(cpos);
        cpos = line.find('#');
        if (cpos != std::string::npos) line.erase(cpos);

        line = trim(line);
        if (line.empty()) continue;

        auto sep = line.find(':');
        if (sep == std::string::npos) {
            LOG_WARN("Config: '%s' line %d: missing ':' separator", path.c_str(), lineno);
            continue;
        }

        std::string key   = trim(line.substr(0, sep));
        std::string value = trim(line.substr(sep + 1));
        data_[key] = value;
    }

    LOG_INFO("Config: loaded '%s' (%zu keys)", path.c_str(), data_.size());
    return true;
}

std::string Config::get_str(const std::string& key, const std::string& def) const
{
    auto it = data_.find(key);
    return it != data_.end() ? it->second : def;
}

int Config::get_int(const std::string& key, int def) const
{
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    try { return std::stoi(it->second); }
    catch (...) { return def; }
}

bool Config::get_bool(const std::string& key, bool def) const
{
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    const auto& v = it->second;
    if (v == "yes" || v == "true" || v == "1") return true;
    if (v == "no"  || v == "false"|| v == "0") return false;
    return def;
}

bool Config::has(const std::string& key) const
{
    return data_.count(key) > 0;
}

std::string Config::trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}
