#pragma once
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  Config — INI-style config reader (rAthena-compatible format)
//
//  Format:
//    // comment
//    key: value
//    key2: value2
// ─────────────────────────────────────────────────────────────────────────────

class Config
{
public:
    bool load(const std::string& path);

    std::string get_str  (const std::string& key, const std::string& def = "") const;
    int         get_int  (const std::string& key, int def = 0) const;
    bool        get_bool (const std::string& key, bool def = false) const;

    bool has(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> data_;
    static std::string trim(const std::string& s);
};
