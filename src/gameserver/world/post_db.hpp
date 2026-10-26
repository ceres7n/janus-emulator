#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PostSetupDb — db/post_setup_db.yml (the original PostSetupInfo): the post box's seven settings
//  (retail GameInfoMgr u16 table, getter sub_7A01B0(i); 0 for a row it lacks).
// ─────────────────────────────────────────────────────────────────────────────
class PostSetupDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("PostSetupDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        int type = -1, value = 0;
        size_t n = 0;
        auto commit = [&]() { if (type >= 0 && type < 7) { v_[type] = static_cast<uint16_t>(value); ++n; } type = -1; value = 0; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (line.compare(ns, 2, "- ") == 0) { commit(); body = line.substr(ns + 2); }
            else if (ns >= 4) body = line.substr(ns);
            else continue;
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            const std::string k = body.substr(0, c);
            if (k == "SetupType") type = std::atoi(body.c_str() + c + 1);
            else if (k == "Value") value = std::atoi(body.c_str() + c + 1);
        }
        commit();
        LOG_INFO("PostSetupDb: loaded %zu settings from %s", n, path.c_str());
        return n > 0;
    }

    // sub_7A01B0
    uint16_t get(int i) const { return (i >= 0 && i < 7) ? v_[i] : 0; }
    // sub_7A01E0: the fee rate of a post sub type — 1 the item fee (3), 2 / 3 the trade fee (4), / 10000.
    float fee_rate(int sub_type) const
    {
        if (sub_type == 1) return static_cast<float>(static_cast<double>(get(3)) / 10000.0);
        if (sub_type == 2 || sub_type == 3) return static_cast<float>(static_cast<double>(get(4)) / 10000.0);
        return 0.0f;
    }

private:
    uint16_t v_[7]{ 7, 7, 1, 10, 10, 500, 5 };
};

// ─────────────────────────────────────────────────────────────────────────────
//  LevelUpGiftDb — db/post_levelup_gift_db.yml (the original PostBoxLevelUpGiftInfo): the GM gift a
//  character gets on reaching a level (retail sLevelUpGiftInfo list, walked by sub_76BE80).
// ─────────────────────────────────────────────────────────────────────────────
class LevelUpGiftDb
{
public:
    struct Item { uint32_t id{ 0 }; int count{ 0 }; };
    struct Gift {
        int id{ 0 }, level{ 0 };
        uint32_t chr_type{ 0 };          // the races it goes to: bit (type - 1)
        std::string sender, title, message;
        std::vector<Item> items;
    };

    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("LevelUpGiftDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        Gift g;
        bool in = false;
        auto commit = [&]() { if (in && g.id > 0) gifts_.push_back(g); g = Gift{}; in = false; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (ns == 2 && line.compare(ns, 2, "- ") == 0) { commit(); in = true; body = line.substr(ns + 2); }
            else if (ns == 6 && line.compare(ns, 2, "- ") == 0) { if (in) g.items.push_back(Item{}); body = line.substr(ns + 2); }
            else if (in && ns >= 4) body = line.substr(ns);
            else { if (line.compare(ns, 7, "Footer:") == 0) commit(); continue; }
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            const std::string k = body.substr(0, c);
            const std::string v = yaml_scalar(body.substr(c + 1));
            if (k == "ID") g.id = std::atoi(v.c_str());
            else if (k == "Level") g.level = std::atoi(v.c_str());
            else if (k == "ChrType") g.chr_type = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "Sender") g.sender = v;
            else if (k == "Title") g.title = v;
            else if (k == "Message") g.message = v;
            else if (k == "ItemId" && !g.items.empty()) g.items.back().id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "ItemCount" && !g.items.empty()) g.items.back().count = std::atoi(v.c_str());
        }
        commit();
        LOG_INFO("LevelUpGiftDb: loaded %zu level-up gifts from %s", gifts_.size(), path.c_str());
        return !gifts_.empty();
    }

    // The gifts of a level, in the table's order.
    std::vector<const Gift*> of_level(int level) const
    {
        std::vector<const Gift*> out;
        for (const Gift& g : gifts_) if (g.level == level) out.push_back(&g);
        return out;
    }

private:
    std::vector<Gift> gifts_;
};
