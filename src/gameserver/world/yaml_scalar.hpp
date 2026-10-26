#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  The value of a `Key: value` line of the project's flat db/*_db.yml files.
//  Strings are plain (unquoted) unless YAML would mis-parse them; then they are
//  single-quoted with '' for a quote inside. A plain value ends at " #" (comment).
// ─────────────────────────────────────────────────────────────────────────────

inline std::string yaml_scalar(const std::string& raw)
{
    size_t a = raw.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    if (raw[a] == '\'') {
        std::string out;
        for (size_t i = a + 1; i < raw.size(); ++i) {
            if (raw[i] == '\'') {
                if (i + 1 < raw.size() && raw[i + 1] == '\'') { out += '\''; ++i; continue; }
                break;
            }
            out += raw[i];
        }
        return out;
    }
    if (raw[a] == '"') {
        size_t b = raw.find('"', a + 1);
        return raw.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    }
    size_t e = raw.find(" #", a);
    std::string out = raw.substr(a, e == std::string::npos ? std::string::npos : e - a);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
