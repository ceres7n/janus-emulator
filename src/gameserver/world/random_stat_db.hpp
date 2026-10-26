#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  RandomStatDb — a new rare item's random stats and compound slots (retail DistrTblInfoMgr,
//  GameInfoMgr dword_F03264), from db/random_stat_import_db.yml (RandomStatImportTable: the
//  curves) and db/random_stat_db.yml (RandomStatValueTable: per item, each stat's range + curve).
//
//  Curve (sub_616E70): the weights Step1, Step10..Step100 become 101 weights — straight lines
//  between the non-zero steps, a zero step stretching the next line over it — then a table of
//  10000 point indexes where point k fills 10000*w[k]/total entries (float bookkeeping, an entry
//  more when the running fraction reaches 1), entry 9999 = 100.
//  Values (sub_617590 / MixSlot sub_618350): 101 values (int)(Min + k*step) with the float
//  step (Max-Min)/100 accumulated in a float; value 100 = Max.
//  A roll (sub_617860 / sub_618640) = value[curve[rand() % 10000]].
//  The x87 arithmetic of the retail (53-bit precision, float stores) is reproduced with explicit
//  double expressions rounded to float where the retail stores a float.
// ─────────────────────────────────────────────────────────────────────────────

struct RandomStatRoll { int16_t id; int16_t value; };   // option type 10, id 10000..10012

class RandomStatDb
{
public:
    bool load(const std::string& stat_path, const std::string& import_path)
    {
        load_curves(import_path);
        load_stats(stat_path);
        LOG_INFO("RandomStatDb: %zu curves, %zu items with random stats, %zu with compound slots",
                 curves_.size(), stats_.size(), slots_.size());
        return !curves_.empty() && !stats_.empty();
    }

    // sub_617860: one roll per stat of the item, in option id order. 0 rolls when the item has no
    // entry or when any of its curves is unknown (the retail returns 0 then: items whose import name
    // is not a curve, e.g. '22' / '28' in the original data, roll nothing).
    int roll_stats(uint32_t item_id, std::vector<RandomStatRoll>& out) const
    {
        auto it = stats_.find(item_id);
        if (it == stats_.end()) return 0;
        std::vector<RandomStatRoll> rolled;
        for (const auto& [id, e] : it->second) {
            auto c = curves_.find(e.import);
            if (c == curves_.end()) return 0;
            const int r = std::rand() % 10000;                       // sub_41ED70(10000)
            const int k = c->second[r];
            rolled.push_back({ static_cast<int16_t>(id), static_cast<int16_t>(e.value[k]) });
        }
        out.insert(out.end(), rolled.begin(), rolled.end());
        return static_cast<int>(rolled.size());
    }

    // sub_6184A0 (DistrTblInfoMgr::GetCustomRandomValue, the mature compound's lucky roll): the
    // curve point 0..100 of a rand(10000) roll, or -1 when the curve is unknown. The caller rescales:
    // value = lo + (hi - lo) * k / 100 (the retail works on the Min/Max of the Maturing_Compound row
    // plus the aid bonuses; a missing curve answers -32768 there — our caller falls back to the floor).
    int curve_point(const std::string& import, int roll) const
    {
        auto c = curves_.find(import);
        if (c == curves_.end()) return -1;
        return c->second[static_cast<size_t>(roll % 10000)];
    }

    // sub_630B90 -> sub_618640: the item's compound slot count, or 127 when it has none (the caller
    // then uses the item's CompoundSlot, sub_6EC2B0). The roll happens even without an entry.
    int roll_mix_slot(uint32_t item_id) const
    {
        if (slots_.empty() && stats_.empty()) return 0;              // this[159]: the tables are loaded
        const int r = std::rand() % 10000;
        auto it = slots_.find(item_id);
        if (it == slots_.end()) return 127;
        auto c = curves_.find(it->second.import);
        if (c == curves_.end()) return 127;
        return it->second.value[c->second[r]];
    }

private:
    struct Entry { int32_t value[101]{}; std::string import; };
    std::unordered_map<std::string, std::vector<uint16_t>> curves_;
    std::unordered_map<uint32_t, std::map<int, Entry>> stats_;     // item -> option id -> entry
    std::unordered_map<uint32_t, Entry> slots_;                     // item -> MixSlot entry

    // sub_616E70
    static std::vector<uint16_t> build_curve(const uint16_t steps[11])
    {
        uint16_t w[101]{};
        int total = steps[0];
        float from = static_cast<float>(steps[0]);
        int base = 0, gap = 1;
        w[0] = steps[0];
        for (int i = 1; i < 11; ++i) {
            if (steps[i]) {
                const int span = 10 * gap;
                for (int j = 1; j <= span && base + j <= 100; ++j) {
                    w[base + j] = static_cast<uint16_t>(static_cast<int>(
                        (static_cast<double>(steps[i]) - static_cast<double>(from)) / static_cast<double>(span)
                        * static_cast<double>(j) + static_cast<double>(from)));
                    total += w[base + j];
                }
                from = static_cast<float>(steps[i]);
                base += span;
                gap = 1;
            } else {
                ++gap;
            }
        }
        std::vector<uint16_t> s(10000, 0);
        if (total <= 0) { s[9999] = 100; return s; }
        float acc = 0.0f;
        for (uint16_t k = 0; k < 101; ++k) {
            const float part = static_cast<float>(static_cast<double>(10000 * w[k]) / static_cast<double>(total));
            uint16_t n = static_cast<uint16_t>(static_cast<int>(part));
            const uint16_t at = static_cast<uint16_t>(static_cast<int>(acc));
            const double frac = static_cast<double>(acc) + static_cast<double>(part)
                              - static_cast<double>(at + static_cast<uint16_t>(static_cast<int>(part)));
            if (frac >= 1.0) ++n;
            for (int i = 0; i < n && at + i < 10000; ++i) s[at + i] = k;   // wmemset(&S[(u16)acc], k, n)
            acc = static_cast<float>(static_cast<double>(acc) + static_cast<double>(part));
        }
        s[9999] = 100;
        return s;
    }

    // sub_617590 / sub_618350
    static void build_values(int lo, int hi, int32_t out[101])
    {
        const float step = static_cast<float>(static_cast<double>(hi - lo) / 100.0);
        float acc = 0.0f;
        for (int i = 0; i < 101; ++i) {
            out[i] = static_cast<int32_t>(static_cast<double>(lo) + static_cast<double>(acc));
            acc = static_cast<float>(static_cast<double>(acc) + static_cast<double>(step));
        }
        out[100] = hi;
    }

    template <typename Fn>
    static bool read_records(const std::string& path, Fn on_record)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("RandomStatDb: cannot open %s", path.c_str()); return false; }
        std::unordered_map<std::string, std::string> rec;
        bool in = false;
        std::string line;
        auto commit = [&]() { if (in) on_record(rec); rec.clear(); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (line.compare(ns, 2, "- ") == 0) { commit(); in = true; body = line.substr(ns + 2); }
            else if (in && ns >= 4)              body = line.substr(ns);
            else if (line.compare(ns, 7, "Footer:") == 0) { commit(); in = false; continue; }
            else continue;
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            rec[body.substr(0, c)] = yaml_scalar(body.substr(c + 1));
        }
        commit();
        return true;
    }

    void load_curves(const std::string& path)
    {
        static const char* const kSteps[11] = { "Step1", "Step10", "Step20", "Step30", "Step40", "Step50",
                                                "Step60", "Step70", "Step80", "Step90", "Step100" };
        read_records(path, [&](const std::unordered_map<std::string, std::string>& r) {
            auto n = r.find("Name");
            if (n == r.end() || n->second.empty()) return;
            uint16_t steps[11]{};
            for (int i = 0; i < 11; ++i) {
                auto v = r.find(kSteps[i]);
                if (v != r.end()) steps[i] = static_cast<uint16_t>(static_cast<int16_t>(std::atoi(v->second.c_str())));
            }
            curves_.emplace(n->second, build_curve(steps));            // the first curve of a name wins
        });
    }

    void load_stats(const std::string& path)
    {
        // off_AAE5E8: the stat names; option id 10000 + index (sub_6187B0).
        static const char* const kStat[13] = { "AP", "AC", "DX", "MP", "MA", "MD", "WT", "DA", "LK", "HP", "DP",
                                               "HV", "GunAp" };
        read_records(path, [&](const std::unordered_map<std::string, std::string>& r) {
            auto idf = r.find("ItemId");
            if (idf == r.end()) return;
            const uint32_t item = static_cast<uint32_t>(std::strtoul(idf->second.c_str(), nullptr, 10));
            auto num = [&](const std::string& k) {
                auto v = r.find(k);
                return v == r.end() ? 0 : static_cast<int>(static_cast<int16_t>(std::atoi(v->second.c_str())));
            };
            std::map<int, Entry> m;
            for (int i = 0; i < 13; ++i) {
                auto imp = r.find(std::string(kStat[i]) + "Import");
                if (imp == r.end() || imp->second.empty()) continue;
                Entry e;
                build_values(num(std::string(kStat[i]) + "Min"), num(std::string(kStat[i]) + "Max"), e.value);
                e.import = imp->second;
                m.emplace(10000 + i, e);
            }
            if (!m.empty()) stats_.emplace(item, std::move(m));             // the first row of an id wins
            auto ms = r.find("MixSlotImport");
            if (ms != r.end() && !ms->second.empty()) {
                Entry e;
                build_values(num("MixSlotMin"), num("MixSlotMax"), e.value);
                e.import = ms->second;
                slots_.emplace(item, e);
            }
        });
    }
};
