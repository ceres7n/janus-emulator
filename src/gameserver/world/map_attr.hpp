#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  MapAttrDb — the zone attribute grid of each map (retail ZoneMapAttr, Init sub_820740): the
//  map's .md3 layer of type 1, one byte per 16x16-unit cell (w x h cells, index y*w + x;
//  1 = blocked). The retail GameServer loads the zone's .md3 (sub_8C3240, header 0x60 bytes =
//  24 x u32 with [0] 'MDN_' and [17] the layer count; each layer = {u32 type, u32 w, u32 h,
//  u32 cellW, u32 cellH, w*h bytes}, sub_8C2DD0) and copies layer 1 into byte 0 of its cells
//  (sub_820960). Dropped items are placed on cells whose byte is not 1 (sub_422200).
//  Byte 3 of a cell is its height: the .md3 layer of type 4, or 5 everywhere when the map has
//  none (ZoneMapAttr::Init -> sub_820AD0 / sub_820B30(5)); a gun shot needs a line of sight over
//  it (sub_5E69C0). Byte 1 is the cell's ground nature: the MapAttr Attr of its layer-3 byte
//  (sub_8209C0, db/map_attr_db.yml; no layer 3 -> 0, "not found groundAttribute") — a drill cannot
//  dig a nature-6 cell and a drill of another GroundNature costs twice its life. Byte 2 is the
//  .md3 layer of type 2, copied as it is (sub_820900; 0 when the map has none): a player may not
//  start on a cell whose byte 2 is 1 (sub_810620).
//
//  The files are db/maps_db.yml FileName (data\map\*.md3) found by file name in `map_path`
//  (conf, default data/map); each map is read once, on its first use.
// ─────────────────────────────────────────────────────────────────────────────

struct MapGrid {
    int16_t w = 0, h = 0;
    std::vector<uint8_t> cell;
    std::vector<uint8_t> nature;   // byte 1 of the retail cell (layer 3 through MapAttr, default 0)
    std::vector<uint8_t> layer2;   // byte 2 of the retail cell (layer 2, default 0)
    std::vector<uint8_t> height;   // byte 3 of the retail cell (layer 4, default 5)
    // The .md3 points Zone::BuildPoints (sub_8113C0) files for the monster regeneration: type 3 ->
    // the zone's spawn list (+0x33F8, at most 1024, sub_8154F0); type 6 with id < 64 -> the list of
    // region `id` (+0x77C0 + 0x804*id, at most 128, sub_815910); type 1 -> the start/portal map
    // (+0x31B4), whose first point is the last fallback of a monster position (sub_858A50).
    struct Pt { int16_t x = 0, y = 0; };
    std::vector<Pt> spawn3;
    std::vector<Pt> region6[64];
    std::vector<Pt> start1;
    // type 4 -> the NPC points by id (+0x7640, sub_815350; NpcManager sub_85DE20 looks PosId up,
    // sub_810EA0); the zone warns past 48.
    std::unordered_map<uint32_t, Pt> npc4;
    // type 5 -> the shop points by id (+0x7600; ShopManager::CreateShop sub_882CF0 looks the shop's
    // PosId up, sub_810E40).
    std::unordered_map<uint32_t, Pt> shop5;
    // type 10 -> the skill shop points by id (+0x76C0; SkillShopManager::CreateSkillShop sub_883130
    // looks the master's PosId up, sub_810F60).
    std::unordered_map<uint32_t, Pt> skill10;
    // The portals: the .md3 regions of type 1 (Zone::BuildRegions sub_811640 -> the zone's portal
    // list +12080, 16 bytes each: {id, x1, y1, x2, y2, zoneTo = the region's param}). The region
    // id is the connection id the destination's arrival point carries (sub_810CF0).
    struct Portal { uint32_t id = 0; int zone_to = 0; int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
    std::vector<Portal> portals;
    // The map's bounds: .md3 header [6..9] {x1, y1, x2, y2} (the md3 object +6296, sub_71B2D0) — a
    // monster's wander destination is clamped into it (sub_71B050).
    int32_t bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;

    // sub_820C20: the cell of (x, y), clamped into the grid.
    uint8_t at(int x, int y) const
    {
        if (x < 0) x = 0; else if (x >= w) x = w - 1;
        if (y < 0) y = 0; else if (y >= h) y = h - 1;
        return cell[static_cast<size_t>(w) * static_cast<size_t>(y) + static_cast<size_t>(x)];
    }
    uint8_t height_at(int x, int y) const
    {
        if (x < 0) x = 0; else if (x >= w) x = w - 1;
        if (y < 0) y = 0; else if (y >= h) y = h - 1;
        return height[static_cast<size_t>(w) * static_cast<size_t>(y) + static_cast<size_t>(x)];
    }

    uint8_t nature_at(int x, int y) const
    {
        if (x < 0) x = 0; else if (x >= w) x = w - 1;
        if (y < 0) y = 0; else if (y >= h) y = h - 1;
        return nature[static_cast<size_t>(w) * static_cast<size_t>(y) + static_cast<size_t>(x)];
    }

    // sub_434300: the height of the cell holding the point (x, y) — C int16 division by 16
    // (sub_422100), then clamped.
    uint8_t height_of(int16_t x, int16_t y) const { return height_at(x / 16, y / 16); }
    // sub_434300 + byte 1: the ground nature of the cell holding the point (x, y).
    uint8_t nature_of(int16_t x, int16_t y) const { return nature_at(x / 16, y / 16); }
    // MapItemManager sub_850640: a cell a buried item may lie in — not blocked (byte 0 != 1) and its
    // ground not undiggable (byte 1 != 6).
    bool diggable_cell(int x, int y) const { return at(x, y) != 1 && nature_at(x, y) != 6; }

    // sub_810940 + sub_810620(pos, 1): a point a player may start on (the login, Zone::PrepareToStartPlayer)
    // — inside the map (0 < x < 16w, 0 < y < 16h), on a cell (C int16 division by 16, sub_422100) whose
    // byte 0 and byte 2 are not 1, and strictly inside no portal region (sub_810C00).
    bool start_point_ok(int32_t px, int32_t py) const
    {
        const int16_t x = static_cast<int16_t>(px), y = static_cast<int16_t>(py);
        if (x <= 0 || y <= 0 || x >= 16 * w || y >= 16 * h) return false;
        const size_t i = static_cast<size_t>(w) * static_cast<size_t>(y / 16) + static_cast<size_t>(x / 16);
        if (cell[i] == 1 || layer2[i] == 1) return false;
        for (const Portal& p : portals)
            if (x > p.x1 && x < p.x2 && y > p.y1 && y < p.y2) return false;
        return true;
    }

    // sub_5E69C0(zone, from, to, 1): the line of sight of a gun shot. From the shooter's point to
    // the target's, in 16-unit steps along the longer axis (|dy/dx| > 1 as a float, sub_57F5A0 =
    // fabsf; dx == 0 walks y), the other coordinate by integer division; the highest cell height
    // met on the way (each step checks the bound BEFORE moving, so the last step may pass the
    // target) is compared with the two ends (sub_5E6C70 type 1): visible when the shooter's cell is
    // at least that high, or the obstacle stands at most 3 above the ends' sum (2*max - a - b <= 3).
    bool line_of_sight(int16_t x1, int16_t y1, int16_t x2, int16_t y2) const
    {
        int16_t cx = x1, cy = y1;
        const int16_t dx = static_cast<int16_t>(x2 - x1);
        const int16_t dy = static_cast<int16_t>(y2 - y1);
        int top = 0;
        auto see = [&]() { const int v = height_of(cx, cy); if (v > top) top = v; };
        if (dx == 0) {
            while (cy != y2) {
                if (dy < 0) { cy = static_cast<int16_t>(cy - 16); if (y2 > cy) break; }
                else        { cy = static_cast<int16_t>(cy + 16); if (y2 < cy) break; }
                see();
            }
        } else if (std::fabs(static_cast<float>(static_cast<double>(dy) / static_cast<double>(dx))) > 1.0) {
            for (;;) {
                if (dy < 0) { if (cy <= y2) break; cy = static_cast<int16_t>(cy - 16); }
                else        { if (cy >= y2) break; cy = static_cast<int16_t>(cy + 16); }
                cx = static_cast<int16_t>(dx * (cy - y1) / dy + x1);
                see();
            }
        } else {
            for (;;) {
                if (dx < 0) { if (cx <= x2) break; cx = static_cast<int16_t>(cx - 16); }
                else        { if (cx >= x2) break; cx = static_cast<int16_t>(cx + 16); }
                cy = static_cast<int16_t>(dy * (cx - x1) / dx + y1);
                see();
            }
        }
        const int a = height_of(x1, y1), b = height_of(x2, y2);
        return a - top >= 0 || top + top - a - b <= 3;
    }

    // sub_893F50(zone, from, to): how far a knocked-back one gets from (x1, y1) toward (x2, y2). The same walk as
    // the line of sight (16-unit steps along the longer axis, |dy/dx| > 1 as a float — fabsf, sub_57F5A0 —, the
    // bound checked before each step so the last step may pass the end; dx == 0 walks y and stops at the end); a
    // step onto a blocked cell (byte 0 == 1, sub_422100 -> the map's vtbl+12) stops it. The last free point keeps
    // only the walked axis — the other stays the start's (retail: the y-major walk never updates x, the x-major
    // one never y). Without a grid (`g` null) nothing blocks.
    static void walk_free(const MapGrid* g, int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t& ox, int16_t& oy)
    {
        int16_t cx = x1, cy = y1;
        ox = x1; oy = y1;
        const int16_t dx = static_cast<int16_t>(x2 - x1);
        const int16_t dy = static_cast<int16_t>(y2 - y1);
        auto blocked = [&]() { return g && g->at(cx / 16, cy / 16) == 1; };
        if (dx == 0) {
            while (cy != y2) {
                if (dy < 0) { cy = static_cast<int16_t>(cy - 16); if (y2 > cy) break; }
                else        { cy = static_cast<int16_t>(cy + 16); if (y2 < cy) break; }
                if (blocked()) break;
                oy = cy;
            }
        } else if (std::fabs(static_cast<float>(static_cast<double>(dy) / static_cast<double>(dx))) > 1.0) {
            for (;;) {
                if (dy < 0) { if (cy <= y2) break; cy = static_cast<int16_t>(cy - 16); }
                else        { if (cy >= y2) break; cy = static_cast<int16_t>(cy + 16); }
                cx = static_cast<int16_t>(dx * (cy - y1) / dy + x1);
                if (blocked()) break;
                oy = cy;
            }
        } else {
            for (;;) {
                if (dx < 0) { if (cx <= x2) break; cx = static_cast<int16_t>(cx - 16); }
                else        { if (cx >= x2) break; cx = static_cast<int16_t>(cx + 16); }
                cy = static_cast<int16_t>(dy * (cx - x1) / dx + y1);
                if (blocked()) break;
                ox = cx;
            }
        }
    }

    // sub_422200: a free cell around (cx, cy) (cell coordinates) for a dropped item. A square spiral
    // from that cell in a random direction (rand() % 4; steps (0,-1) (-1,0) (0,1) (1,0), table
    // word_98375C): the first rings (legs 1,1,2,2,...,inner-1) are only walked; then, after skipping
    // rand() % (R*(R-1)/2) cells, the first cell inside the R x R box around the start
    // (R = min(max(w,h), outer), clipped to the map) whose byte is not 1 and that `taken` (this
    // batch's cells — the retail marks them in the grid and clears them afterwards) does not hold
    // wins. Every step wraps modulo the map size (C int16 %: a negative coordinate stays negative
    // and is never inside the box). Returns false when the walk ends without a free cell.
    bool find_drop_cell(int inner, int outer, int16_t& cx, int16_t& cy,
                        const std::vector<std::pair<int16_t, int16_t>>& taken) const
    {
        static constexpr int16_t kDir[4][2] = { { 0, -1 }, { -1, 0 }, { 0, 1 }, { 1, 0 } };
        const int sx = w, sy = h;
        if (sx <= 0 || sy <= 0) return false;
        int16_t x = cx, y = cy;
        int dir = std::rand() % 4;
        const int big = (sx > sy) ? sx : sy;
        const int r = (big >= outer) ? outer : big;
        const int half = r / 2;
        const int16_t max_y = static_cast<int16_t>((sy < y + half) ? sy : y + half);
        const int16_t max_x = static_cast<int16_t>((sx < x + half) ? sx : x + half);
        const int16_t min_y = static_cast<int16_t>((y - half > 0) ? y - half : 0);
        const int16_t min_x = static_cast<int16_t>((x - half > 0) ? x - half : 0);
        auto step = [&](int d) {
            x = static_cast<int16_t>(x + kDir[d][0]);
            y = static_cast<int16_t>(y + kDir[d][1]);
            x = static_cast<int16_t>(x % sx);
            y = static_cast<int16_t>(y % sy);
        };
        int i = 1;
        for (; i < inner; ++i)
            for (int j = 0; j < 2; ++j) {
                for (int k = 0; k < i; ++k) step(dir);
                dir = (dir + 1) & 3;
            }
        const int first = (inner >= 1) ? 1 : inner;
        const int cells = (r - first) * (r - 1 + first);
        int skip = std::rand() % ((cells <= 1) ? 1 : cells / 2);
        for (; i < r; ++i)
            for (int m = 0; m < 2; ++m) {
                for (int n = 0; n < i; ++n) {
                    if (skip > 0) {
                        --skip;
                    } else if (x >= min_x && y >= min_y && x < max_x && y < max_y) {   // sub_422610
                        const bool busy = std::find(taken.begin(), taken.end(), std::make_pair(x, y)) != taken.end();
                        if (at(x, y) != 1 && !busy) { cx = x; cy = y; return true; }
                    }
                    step(dir);
                }
                dir = (dir + 1) & 3;
            }
        return false;
    }
};

class MapAttrDb
{
public:
    void set_dir(const std::string& dir) { dir_ = dir; }

    // db/map_attr_db.yml (MapAttr, sub_8209C0: a 17-entry table, Type -> Attr; an absent Type reads
    // 0). Load it before the first grid.
    bool load_attr_table(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MapAttrDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        int type = -1, attr = 0, rows = 0;
        bool in_record = false;
        auto commit = [&]() {
            if (in_record && type >= 0 && type < 17) { attr_[type] = static_cast<uint8_t>(attr); ++rows; }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string s = line.substr(ns);
            if (s.compare(0, 2, "- ") == 0) { commit(); in_record = true; type = -1; attr = 0; s = s.substr(2); }
            else if (s.compare(0, 7, "Footer:") == 0) { commit(); in_record = false; continue; }
            else if (!in_record || ns < 4) continue;
            const size_t c = s.find(':');
            if (c == std::string::npos) continue;
            const std::string k = s.substr(0, c);
            if (k == "Type") type = std::atoi(s.c_str() + c + 1);
            else if (k == "Attr") attr = std::atoi(s.c_str() + c + 1);
        }
        commit();
        LOG_INFO("MapAttrDb: %d MapAttr rows (ground natures) from %s", rows, path.c_str());
        return rows > 0;
    }

    // The grid of map `map_id` whose MapInfoEx FileName is `md3_file`; nullptr when the file or its
    // type-1 layer is missing (logged once).
    const MapGrid* grid(int map_id, const std::string& md3_file)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(map_id);
        if (it != cache_.end()) return it->second.get();
        std::unique_ptr<MapGrid> g = load(md3_file);
        const MapGrid* out = g.get();
        cache_.emplace(map_id, std::move(g));
        return out;
    }

private:
    std::string dir_{ "data/map" };
    uint8_t     attr_[17]{};          // MapAttr Type -> Attr (sub_8209C0's v14[17])
    std::mutex  mutex_;
    struct PendingPoint { uint32_t id, type; MapGrid::Pt pt; };
    std::vector<PendingPoint> pending_points_;   // (load's scratch, under mutex_)
    std::vector<MapGrid::Portal> pending_portals_;
    std::unordered_map<int, std::unique_ptr<MapGrid>> cache_;

    std::unique_ptr<MapGrid> load(const std::string& md3_file)
    {
        // "data\map\map_sq00.md3" -> "<dir>/map_sq00.md3"
        const size_t cut = md3_file.find_last_of("\\/");
        const std::string base = (cut == std::string::npos) ? md3_file : md3_file.substr(cut + 1);
        if (base.empty()) return nullptr;
        const std::string path = dir_ + "/" + base;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { LOG_ERROR("MapAttrDb: cannot open %s", path.c_str()); return nullptr; }
        uint32_t hdr[24]{};
        pending_points_.clear();
        pending_portals_.clear();
        std::unique_ptr<MapGrid> out;
        std::vector<uint8_t> heights;                     // layer 4 (the cells' byte 3)
        bool have_heights = false;
        std::vector<uint8_t> ground;                      // layer 3 (MapAttr Type -> the cells' byte 1)
        bool have_ground = false;
        std::vector<uint8_t> second;                      // layer 2 (the cells' byte 2)
        bool have_second = false;
        if (std::fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == 1598964813u) {   // 'MDN_'
            for (uint32_t i = 0; i < hdr[17]; ++i) {
                uint32_t lh[5]{};
                if (std::fread(lh, sizeof(lh), 1, f) != 1) break;
                const uint64_t bytes = static_cast<uint64_t>(lh[1]) * lh[2];
                const bool sane = lh[1] > 0 && lh[2] > 0 && lh[1] < 0x8000 && lh[2] < 0x8000;
                if (lh[0] == 1 && sane && !out) {
                    auto g = std::make_unique<MapGrid>();
                    g->w = static_cast<int16_t>(lh[1]);
                    g->h = static_cast<int16_t>(lh[2]);
                    g->cell.resize(static_cast<size_t>(bytes));
                    if (std::fread(g->cell.data(), 1, g->cell.size(), f) != g->cell.size()) break;
                    out = std::move(g);
                    continue;
                }
                if (lh[0] == 4 && sane && !have_heights) {
                    heights.resize(static_cast<size_t>(bytes));
                    if (std::fread(heights.data(), 1, heights.size(), f) != heights.size()) break;
                    have_heights = true;
                    continue;
                }
                if (lh[0] == 3 && sane && !have_ground) {
                    ground.resize(static_cast<size_t>(bytes));
                    if (std::fread(ground.data(), 1, ground.size(), f) != ground.size()) break;
                    have_ground = true;
                    continue;
                }
                if (lh[0] == 2 && sane && !have_second) {
                    second.resize(static_cast<size_t>(bytes));
                    if (std::fread(second.data(), 1, second.size(), f) != second.size()) break;
                    have_second = true;
                    continue;
                }
                if (std::fseek(f, static_cast<long>(bytes), SEEK_CUR) != 0) break;
            }
            // The regions (0x1C bytes each, 0x20 from version 301: i32 id, type, param, x1, y1,
            // x2, y2) and the points (0x14 / 0x18: u32 id, type, param, x, y) — sub_8C3240 /
            // sub_820E40.
            const long region_sz = (hdr[1] < 301) ? 0x1C : 0x20;
            const long point_sz  = (hdr[1] < 301) ? 0x14 : 0x18;
            if (hdr[18] < 100000 && hdr[19] < 100000) {
                std::vector<uint8_t> reg(static_cast<size_t>(region_sz));
                bool regions_ok = true;
                for (uint32_t i = 0; i < hdr[18]; ++i) {
                    if (std::fread(reg.data(), 1, reg.size(), f) != reg.size()) { regions_ok = false; break; }
                    int32_t r[7];
                    std::memcpy(r, reg.data(), sizeof(r));
                    if (r[1] == 1)                         // BuildRegions case 1: a portal
                        pending_portals_.push_back({ static_cast<uint32_t>(r[0]), r[2],
                                                     static_cast<int16_t>(r[3]), static_cast<int16_t>(r[4]),
                                                     static_cast<int16_t>(r[5]), static_cast<int16_t>(r[6]) });
                }
                if (!regions_ok) hdr[19] = 0;
                std::vector<uint8_t> rec(static_cast<size_t>(point_sz));
                for (uint32_t i = 0; i < hdr[19]; ++i) {
                    if (std::fread(rec.data(), 1, rec.size(), f) != rec.size()) break;
                    uint32_t v[5];
                    std::memcpy(v, rec.data(), sizeof(v));
                    MapGrid::Pt pt{ static_cast<int16_t>(v[3]), static_cast<int16_t>(v[4]) };
                    pending_points_.push_back({ v[0], v[1], pt });
                }
            }
        }
        std::fclose(f);
        if (!out) {
            LOG_ERROR("MapAttrDb: %s has no attribute layer (type 1)", path.c_str());
            return out;
        }
        // sub_820AD0 copies w*h bytes of layer 4 (same grid); none -> sub_820B30(5).
        out->height.assign(out->cell.size(), have_heights ? 0 : 5);
        if (have_heights)
            std::copy_n(heights.begin(), std::min(heights.size(), out->height.size()), out->height.begin());
        // sub_820900: byte 2 of the cell i = layer2[i] for the w*h cells.
        out->layer2.assign(out->cell.size(), 0);
        if (have_second)
            std::copy_n(second.begin(), std::min(second.size(), out->layer2.size()), out->layer2.begin());
        // sub_8209C0: byte 1 of the cell i = attr[layer3[i]] for the w*h cells (a Type past the
        // 17-entry table reads 0 here — the retail stack array has no such entry).
        out->nature.assign(out->cell.size(), 0);
        if (have_ground) {
            const size_t n = std::min(ground.size(), out->nature.size());
            for (size_t i = 0; i < n; ++i) out->nature[i] = (ground[i] < 17) ? attr_[ground[i]] : 0;
        } else {
            LOG_INFO("ZoneMapAttr::Init - not found groundAttribute, zoneFileName:%s", path.c_str());
        }
        for (const PendingPoint& p : pending_points_) {
            if (p.type == 3) {
                if (out->spawn3.size() < 1024) out->spawn3.push_back(p.pt);
            } else if (p.type == 6) {
                if (p.id < 64 && out->region6[p.id].size() < 128) out->region6[p.id].push_back(p.pt);
            } else if (p.type == 1) {
                out->start1.push_back(p.pt);
            } else if (p.type == 4) {
                out->npc4.emplace(p.id, p.pt);
            } else if (p.type == 5) {
                out->shop5.emplace(p.id, p.pt);
            } else if (p.type == 10) {
                out->skill10.emplace(p.id, p.pt);
            }
        }
        pending_points_.clear();
        out->portals = std::move(pending_portals_);
        pending_portals_.clear();
        out->bx1 = static_cast<int32_t>(hdr[6]); out->by1 = static_cast<int32_t>(hdr[7]);
        out->bx2 = static_cast<int32_t>(hdr[8]); out->by2 = static_cast<int32_t>(hdr[9]);
        return out;
    }
};
