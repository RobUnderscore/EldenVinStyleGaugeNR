#include "minimap.h"
#include "config.h"
#include "coop_resync.h"
#include "d3dhook.h"
#include "drop_finder.h"
#include "game_textures.h"
#include "world.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern void Log(const char* fmt, ...);

namespace Minimap {

using namespace World;

// ---- settings ----

static std::atomic<bool> s_On{ true };
static std::atomic<int> s_Opacity{ 100 }, s_Boss{ BOSS_FADE };
static std::once_flag s_SettingsOnce;

static void LoadSettings() {
    std::call_once(s_SettingsOnce, [] {
        char path[MAX_PATH], line[128];
        if (!Config::UserFile("Minimap.ini", path, sizeof(path))) return;
        if (FILE* f = fopen(path, "r")) {
            while (fgets(line, sizeof(line), f)) {
                char key[64];
                int v;
                if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2) continue;
                if (!strcmp(key, "show")) s_On = v != 0;
                else if (!strcmp(key, "opacity")) s_Opacity = (std::clamp)(v, 10, 100);
                else if (!strcmp(key, "boss_fights")) s_Boss = (std::clamp)(v, 0, 2);
            }
            fclose(f);
        }
        Log("Minimap: %s, opacity %d%%, boss fights %d (%s)", s_On ? "on" : "off", s_Opacity.load(), s_Boss.load(), path);
    });
}

static void SaveSettings() {
    char path[MAX_PATH];
    if (!Config::UserFile("Minimap.ini", path, sizeof(path))) return;
    FILE* f = fopen(path, "w");
    if (!f) return Log("Minimap: couldn't save %s", path);
    fprintf(f, "# Minimap (Esc > System > Game Options > Vin-Swag > Minimap)\n");
    fprintf(f, "# show: 1 on, 0 off; opacity: 10..100 (percent); boss_fights: 0 keep it, 1 fade it, 2 hide it\n");
    fprintf(f, "show = %d\nopacity = %d\nboss_fights = %d\n", s_On ? 1 : 0, s_Opacity.load(), s_Boss.load());
    fclose(f);
}

bool Enabled() { LoadSettings(); return s_On; }
void SetEnabled(bool on) { LoadSettings(); s_On = on; SaveSettings(); }
int Opacity() { LoadSettings(); return s_Opacity; }
void SetOpacity(int percent) { LoadSettings(); s_Opacity = (std::clamp)(percent, 10, 100); SaveSettings(); }
int BossFights() { LoadSettings(); return s_Boss; }
void SetBossFights(int mode) { LoadSettings(); s_Boss = (std::clamp)(mode, 0, 2); SaveSettings(); }

// ---- the world map's frame ----

constexpr int kTile = 256;            // px per cell, 1 px per metre
constexpr float kRadius = 125.0f;     // px at 1080p
constexpr float kRange = 100.0f;      // metres from the centre to the rim
constexpr float kMarginX = 48.0f, kMarginY = 47.0f;  // from the top-right corner, px at 1080p

enum MapKind { OVERWORLD = 0, UNDERGROUND = 1, SHADOW = 2, NO_MAP = -1 };
static const char* const kMapNames[] = { "M00", "M01", "M10" };

// Overworld metres -> world-map pixels (Y up); cell (i, j) holds pixels [256 i, 256 i + 256)
static float MapX(float x) { return x + kTile / 2.0f - 28.0f * kTile; }
static float MapZ(float z) { return z + kTile / 2.0f - 24.0f * kTile; }

// Which world map shows a position: the underground ones (m12) have their own, sharing the overworld's frame
static MapKind KindOf(uint32_t sourceMap, const WPos& at) {
    if (at.space == 61) return SHADOW;
    if (at.space != 60) return NO_MAP;
    return (sourceMap >> 24) == 12 ? UNDERGROUND : OVERWORLD;
}

// ---- MapTiles.txt and the tile thread ----

struct TileRef {
    uint64_t offset;
    uint32_t size;
};
struct CellKey {
    int map, x, y;
    bool operator<(const CellKey& o) const { return map != o.map ? map < o.map : x != o.x ? x < o.x : y < o.y; }
    bool operator==(const CellKey& o) const { return map == o.map && x == o.x && y == o.y; }
};

static std::map<CellKey, uint32_t> s_Masks;                        // cell -> the map fragments it has variants for
static std::map<std::pair<CellKey, uint32_t>, TileRef> s_Tiles;    // (cell, fragments shown) -> where in Data0.bdt
static std::atomic<int> s_IndexState{ 0 };                          // 0 not read, 1 ready, 2 failed

static bool ReadIndex() {
    char path[MAX_PATH];
    GameTextures::PathNextToDll("MapTiles.txt", path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) { Log("Minimap: MapTiles.txt missing: no map art"); return false; }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string bdt = exe;
    bdt = bdt.substr(0, bdt.find_last_of("\\/") + 1) + "Data0.bdt";
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    uint64_t bdtSize = GetFileAttributesExA(bdt.c_str(), GetFileExInfoStandard, &attr)
        ? ((uint64_t)attr.nFileSizeHigh << 32 | attr.nFileSizeLow) : 0;
    char line[256];
    bool sizeOk = false;
    while (fgets(line, sizeof(line), f)) {
        char map[8];
        int x, y;
        unsigned long long a, b;
        unsigned flags;
        if (line[0] == 'B' && sscanf(line + 2, "%llu", &a) == 1) sizeOk = a == bdtSize;
        else if (line[0] == 'K' && sscanf(line + 2, "%7s %d %d %llu", map, &x, &y, &a) == 4) {
            for (int m = 0; m < 3; m++)
                if (!strcmp(map, kMapNames[m])) s_Masks[{ m, x, y }] = (uint32_t)a;
        } else if (line[0] == 'T' && sscanf(line + 2, "%7s %d %d %x %llu %llu", map, &x, &y, &flags, &a, &b) == 6) {
            for (int m = 0; m < 3; m++)
                if (!strcmp(map, kMapNames[m])) s_Tiles[{ { m, x, y }, flags }] = { a, (uint32_t)b };
        }
    }
    fclose(f);
    if (!sizeOk) {
        Log("Minimap: Data0.bdt isn't the one MapTiles.txt was built from (a game update?): no map art");
        return false;
    }
    Log("Minimap: %zu map tiles indexed", s_Tiles.size());
    return true;
}

using OodleDecompressFn = int64_t(WINAPI*)(const void*, int64_t, void*, int64_t, int, int, int, void*, int64_t, void*, void*,
                                           void*, int64_t, int);

struct Decoded {
    CellKey cell;
    std::vector<uint8_t> dds;  // empty: no tile there
};

static std::mutex s_TileLock;
static std::condition_variable s_TileWake;
static std::vector<CellKey> s_Wanted;       // under s_TileLock: cells the map shows now
static std::deque<Decoded> s_Done;          // under s_TileLock: decoded, waiting for the render thread
static bool s_TileThread = false;

static bool BigEndian32(const uint8_t* p, uint32_t& v) {
    v = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    return true;
}

// The DDS of a tile: DCX (KRAK) -> TPF -> its one texture
static bool DecodeTile(OodleDecompressFn decompress, const std::vector<uint8_t>& dcx, std::vector<uint8_t>& dds) {
    if (dcx.size() < 0x4c || memcmp(dcx.data(), "DCX\0", 4) || memcmp(dcx.data() + 0x28, "KRAK", 4)) return false;
    uint32_t raw, comp;
    BigEndian32(dcx.data() + 0x1c, raw);
    BigEndian32(dcx.data() + 0x20, comp);
    if (comp > dcx.size() - 0x4c || raw > (1u << 24)) return false;
    std::vector<uint8_t> tpf(raw + 64);
    if (decompress(dcx.data() + 0x4c, comp, tpf.data(), raw, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3) != raw) return false;
    if (memcmp(tpf.data(), "TPF\0", 4)) return false;
    uint32_t off, size;
    memcpy(&off, tpf.data() + 0x10, 4);
    memcpy(&size, tpf.data() + 0x14, 4);
    if ((uint64_t)off + size > raw || size < 0x94 || memcmp(tpf.data() + off, "DDS ", 4)) return false;
    dds.assign(tpf.begin() + off, tpf.begin() + off + size);
    return true;
}

static void TileThread() {
    if (!ReadIndex()) {
        s_IndexState = 2;
        return;
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string bdt = exe;
    bdt = bdt.substr(0, bdt.find_last_of("\\/") + 1) + "Data0.bdt";
    HANDLE file = CreateFileA(bdt.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
    auto decompress = oodle ? reinterpret_cast<OodleDecompressFn>(GetProcAddress(oodle, "OodleLZ_Decompress")) : nullptr;
    if (file == INVALID_HANDLE_VALUE || !decompress) {
        Log("Minimap: can't read Data0.bdt or load Oodle: no map art");
        s_IndexState = 2;
        return;
    }
    s_IndexState = 1;
    std::vector<CellKey> done;  // decoded already (the render thread keeps them)
    int failures = 0;
    for (;;) {
        CellKey next{ -1, 0, 0 };
        {
            std::unique_lock<std::mutex> lock(s_TileLock);
            s_TileWake.wait_for(lock, std::chrono::milliseconds(500));
            for (const CellKey& c : s_Wanted)
                if (std::find(done.begin(), done.end(), c) == done.end()) { next = c; break; }
            // forget what has left the view so it can be read again later
            done.erase(std::remove_if(done.begin(), done.end(), [](const CellKey& c) {
                return std::find(s_Wanted.begin(), s_Wanted.end(), c) == s_Wanted.end();
            }), done.end());
        }
        while (next.map >= 0) {
            Decoded d{ next, {} };
            auto mask = s_Masks.find(next);
            uint32_t fragments = mask == s_Masks.end() ? 0 : mask->second;  // the whole map: every fragment
            auto t = s_Tiles.find({ next, fragments });
            if (t == s_Tiles.end()) t = s_Tiles.find({ next, 0u });
            if (t != s_Tiles.end()) {
                std::vector<uint8_t> dcx(t->second.size);
                LARGE_INTEGER at;
                at.QuadPart = (LONGLONG)t->second.offset;
                DWORD got = 0;
                if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, dcx.data(), t->second.size, &got, nullptr) &&
                    got == t->second.size && !DecodeTile(decompress, dcx, d.dds)) {
                    if (failures++ < 5) Log("Minimap: tile %s %d %d didn't decode", kMapNames[next.map], next.x, next.y);
                    d.dds.clear();
                }
            }
            done.push_back(next);
            {
                std::lock_guard<std::mutex> lock(s_TileLock);
                s_Done.push_back(std::move(d));
                next = { -1, 0, 0 };
                for (const CellKey& c : s_Wanted)
                    if (std::find(done.begin(), done.end(), c) == done.end()) { next = c; break; }
            }
        }
    }
}

// ---- what the game thread sees ----

struct Mark {
    float mx, mz;  // world-map pixels
    int kind;      // 0 partner, 1 host, 2 grace, 3 beacon, 4 tracked
    std::string label;
};
struct View {
    bool on = false;
    int map = NO_MAP;
    float mx = 0, mz = 0;  // the player, world-map pixels
    float heading = 0;     // camera, degrees from north
    bool haveHeading = false;
    bool bossBar = false;  // a boss's health bar is up
    std::vector<Mark> marks;
};
static std::mutex s_Lock;
static View s_View;  // under s_Lock

// Beacons: CSMenuManImp (vtable 0x2aa0fc8, found among the statics like the camera) +0x80 -> +0x250 -> +0x338 =
// the map's marker save data (kubilaiswf-minimap, from soarqin's EROverlay notes): slots at +0x8, capacity u32 at
// +0x10; a slot is { s32 id (< 0 empty), f32 x, f32 y (game map coordinates, Y down), u8 map (0, 1, 10), u8 icon }
static constexpr uintptr_t MENU_MAN_VTABLE = 0x2aa0fc8;
// CSFeManImp (vtable 0x2aa2af8): its three boss health bars at +0x5bf0, 0x20 bytes each { s32 name text id, pad,
// u64 the boss's FieldInsHandle } (the game's own getter at 0x76eff0 hands out this + 0x5bf0 + i * 0x20, i <= 2);
// unused ones hold -1
static constexpr uintptr_t FE_MAN_VTABLE = 0x2aa2af8;

// The static holding the object with this vtable (looked for among the statics like the camera's), cached
struct StaticRef {
    uintptr_t vtable;
    const char* what;
    uintptr_t at = 0;
    ULONGLONG searchAt = 0;
};
static StaticRef s_MenuMan{ MENU_MAN_VTABLE, "menu manager" }, s_FeMan{ FE_MAN_VTABLE, "front-end manager" };

static uintptr_t Find(StaticRef& r) {
    if (r.at) {
        uintptr_t m = Ptr(r.at);
        if (m && Rd<uintptr_t>(m) == Base() + r.vtable) return m;
        r.at = 0;
    }
    ULONGLONG now = GetTickCount64();
    if (now < r.searchAt) return 0;
    r.searchAt = now + 30000;
    for (uintptr_t rva = 0x3c00000; rva < 0x4000000; rva += 8) {
        uintptr_t p = Rd<uintptr_t>(Base() + rva);
        if (!Valid(p) || (p >= Base() && p < Base() + 0x6000000)) continue;
        if (Rd<uintptr_t>(p) == Base() + r.vtable) {
            r.at = Base() + rva;
            Log("Minimap: %s at static +0x%llX", r.what, (unsigned long long)rva);
            return p;
        }
    }
    return 0;
}

static uintptr_t MenuMan() { return Find(s_MenuMan); }

static bool BossBarUp() {
    uintptr_t fe = Find(s_FeMan);
    if (!fe) return false;
    static bool s_Was = false;
    bool up = false;
    int32_t name = 0;
    uint64_t handle = 0;
    for (int i = 0; i < 3 && !up; i++) {
        name = Rd<int32_t>(fe + 0x5bf0 + i * 0x20);
        handle = Rd<uint64_t>(fe + 0x5bf0 + i * 0x20 + 8);
        up = name != -1 && name != 0 && handle != ~0ull && handle != 0;
    }
    if (up != s_Was) {
        s_Was = up;
        if (up) Log("Minimap: boss bar up (name %d, handle %016llX)", name, (unsigned long long)handle);
        else Log("Minimap: boss bar down");
    }
    return up;
}

static void Beacons(int map, std::vector<Mark>& out) {
    uintptr_t menu = MenuMan();
    uintptr_t owner = menu ? Ptr(menu + 0x80) : 0;
    uintptr_t view = owner ? Ptr(owner + 0x250) : 0;
    uintptr_t save = view ? Ptr(view + 0x338) : 0;
    uintptr_t slots = save ? Ptr(save + 0x8) : 0;
    if (!slots) return;
    uint32_t cap = std::min<uint32_t>(Rd<uint32_t>(save + 0x10), 10);
    static const int kMenuId[] = { 0, 1, 10 };
    for (uint32_t i = 0; i < cap; i++) {
        uintptr_t s = slots + i * 16;
        float x = Rd<float>(s + 4), y = Rd<float>(s + 8);
        if (Rd<int32_t>(s) < 0 || Rd<uint8_t>(s + 12) != kMenuId[map] || !std::isfinite(x) || !std::isfinite(y)) continue;
        // game map coordinates -> overworld metres -> world-map pixels
        float wx = x - kTile / 2.0f + 28.0f * kTile, wz = (41.0f * kTile - y) - kTile / 2.0f + 24.0f * kTile;
        out.push_back({ MapX(wx), MapZ(wz), 3, std::to_string(i + 1) });
    }
}

static std::vector<int8_t> s_GraceOn;
static ULONGLONG s_NextGraceRead = 0;

void RunFrame() {
    CheckCode();
    LoadSettings();
    if (!s_On) {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_View = View{};
        return;
    }
    if (!Base() || !Ready()) return;
    if (!s_TileThread) {
        s_TileThread = true;
        std::thread(TileThread).detach();
    }
    View v;
    WPos at;
    float phys[3];
    uint32_t block = 0;
    uintptr_t gm = Global(0x3d709a8);  // GameMan: +0x10 set while a loading screen is up
    if ((gm && Rd<uint8_t>(gm + 0x10)) || !Player(at, phys, &block)) {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_View = v;
        return;
    }
    v.on = true;
    v.bossBar = s_Boss != BOSS_KEEP && BossBarUp();
    v.map = KindOf(block, at);
    v.mx = MapX(at.x), v.mz = MapZ(at.z);
    v.haveHeading = CameraHeading(phys, v.heading);
    float reach = kRange * 1.6f;
    auto inReach = [&](float mx, float mz) { return fabsf(mx - v.mx) < reach && fabsf(mz - v.mz) < reach; };

    for (const CoopResync::Partner& p : CoopResync::Partners()) {
        WPos w;
        ToWorld((uint32_t)p.block, p.x, p.y, p.z, w);
        if (v.map == NO_MAP ? w.space != at.space : KindOf((uint32_t)p.block, w) != v.map) continue;
        // a partner off the map's edge still shows on the rim
        v.marks.push_back({ MapX(w.x), MapZ(w.z), p.isHost ? 1 : 0, p.name });
    }
    if (v.map != NO_MAP) {
        ULONGLONG now = GetTickCount64();
        const std::vector<GraceInfo>& graces = Graces();
        if (now >= s_NextGraceRead || s_GraceOn.size() != graces.size()) {
            s_NextGraceRead = now + 2000;
            s_GraceOn.resize(graces.size());
            for (size_t i = 0; i < graces.size(); i++) s_GraceOn[i] = (int8_t)Flag(graces[i].flag);
        }
        for (size_t i = 0; i < graces.size(); i++) {
            const GraceInfo& g = graces[i];
            if (s_GraceOn[i] != 1 || KindOf(g.map, g.pos) != v.map) continue;
            float mx = MapX(g.pos.x), mz = MapZ(g.pos.z);
            if (inReach(mx, mz)) v.marks.push_back({ mx, mz, 2, g.name });
        }
        Beacons(v.map, v.marks);
    }
    WPos track;
    std::string label;
    if (DropFinder::TrackedSpot(track, label) && track.space == at.space) v.marks.push_back({ MapX(track.x), MapZ(track.z), 4, label });

    if (v.map != NO_MAP && s_IndexState.load() == 1) {
        // the cells under the circle
        std::vector<CellKey> want;
        int x0 = (int)floorf((v.mx - kRange * 1.5f) / kTile), x1 = (int)floorf((v.mx + kRange * 1.5f) / kTile);
        int y0 = (int)floorf((v.mz - kRange * 1.5f) / kTile), y1 = (int)floorf((v.mz + kRange * 1.5f) / kTile);
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                if (x >= 0 && y >= 0 && x < 41 && y < 41) want.push_back({ v.map, x, y });
        static int s_OffGridLogs = 0;
        if (want.empty() && s_OffGridLogs++ < 3)
            Log("Minimap: block %08X (%.0f, %.0f) is off the world map's grid (%.0f, %.0f): no map art there", block,
                at.x, at.z, v.mx, v.mz);
        std::lock_guard<std::mutex> lock(s_TileLock);
        if (want != s_Wanted) {
            s_Wanted = want;
            s_TileWake.notify_one();
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_View = std::move(v);
}

// ---- the render thread ----

static constexpr int kSlotsPerRow = 4, kSlots = kSlotsPerRow * kSlotsPerRow;  // a 1024 px atlas of 256 px cells
static uint64_t s_Atlas = 0;
static bool s_AtlasTried = false;
struct Slot {
    CellKey cell{ -1, 0, 0 };
    bool filled = false;  // a tile is there (else the cell has none)
    ULONGLONG used = 0;
};
static Slot s_Slots[kSlots];

static int SlotOf(const CellKey& c) {
    for (int i = 0; i < kSlots; i++)
        if (s_Slots[i].cell == c) return i;
    return -1;
}

static void Upload(const std::vector<CellKey>& wanted) {
    if (!s_Atlas) {
        if (s_AtlasTried) return;
        s_AtlasTried = true;
        s_Atlas = D3DHook::CreateAtlas(kSlotsPerRow * kTile, kSlotsPerRow * kTile, 98 /* BC7 */);
        if (!s_Atlas) Log("Minimap: no atlas texture: no map art");
        return;
    }
    for (int n = 0; n < 2; n++) {
        Decoded d;
        {
            std::lock_guard<std::mutex> lock(s_TileLock);
            if (s_Done.empty()) return;
            d = std::move(s_Done.front());
            s_Done.pop_front();
        }
        if (SlotOf(d.cell) >= 0) continue;
        // the least recently drawn slot the view doesn't need
        int best = -1;
        for (int i = 0; i < kSlots; i++) {
            bool needed = std::find(wanted.begin(), wanted.end(), s_Slots[i].cell) != wanted.end();
            if (needed) continue;
            if (best < 0 || s_Slots[i].used < s_Slots[best].used) best = i;
        }
        if (best < 0) return;
        s_Slots[best] = { d.cell, false, GetTickCount64() };
        if (!d.dds.empty())
            s_Slots[best].filled = D3DHook::UpdateAtlas(s_Atlas, (best % kSlotsPerRow) * kTile, (best / kSlotsPerRow) * kTile,
                                                        d.dds.data(), d.dds.size());
    }
}

// The circle (as a polygon) clipped to an axis-aligned rectangle (Sutherland-Hodgman)
static std::vector<ImVec2> ClipToRect(const std::vector<ImVec2>& poly, ImVec2 lo, ImVec2 hi) {
    std::vector<ImVec2> in = poly, out;
    for (int edge = 0; edge < 4 && !in.empty(); edge++) {
        auto inside = [&](const ImVec2& p) {
            switch (edge) {
            case 0: return p.x >= lo.x;
            case 1: return p.x <= hi.x;
            case 2: return p.y >= lo.y;
            default: return p.y <= hi.y;
            }
        };
        auto cross = [&](const ImVec2& a, const ImVec2& b) {
            float t;
            switch (edge) {
            case 0: t = (lo.x - a.x) / (b.x - a.x); break;
            case 1: t = (hi.x - a.x) / (b.x - a.x); break;
            case 2: t = (lo.y - a.y) / (b.y - a.y); break;
            default: t = (hi.y - a.y) / (b.y - a.y); break;
            }
            return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        };
        out.clear();
        for (size_t i = 0; i < in.size(); i++) {
            const ImVec2& a = in[i];
            const ImVec2& b = in[(i + 1) % in.size()];
            bool ia = inside(a), ib = inside(b);
            if (ia) out.push_back(a);
            if (ia != ib) out.push_back(cross(a, b));
        }
        in.swap(out);
    }
    return in;
}

static void DrawArrow(ImDrawList* dl, ImVec2 c, float deg, float r, ImU32 fill, ImU32 edge) {
    float a = deg / 57.29578f, s = sinf(a), co = cosf(a);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * co - y * s, c.y + x * s + y * co); };
    ImVec2 tip = P(0, -r), l = P(-r * 0.65f, r * 0.7f), m = P(0, r * 0.3f), rt = P(r * 0.65f, r * 0.7f);
    dl->AddTriangleFilled(tip, l, m, fill);
    dl->AddTriangleFilled(tip, m, rt, fill);
    ImVec2 pts[] = { tip, l, m, rt };
    dl->AddPolyline(pts, 4, edge, ImDrawFlags_Closed, 1.5f);
}

void Draw(ImFont* font) {
    View v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        v = s_View;
    }
    static float s_Fade = 1.0f;  // eases to the boss-fight level and back in about half a second
    auto& io = ImGui::GetIO();
    float fadeTo = !v.bossBar ? 1.0f : s_Boss == BOSS_HIDE ? 0.0f : s_Boss == BOSS_FADE ? 0.25f : 1.0f;
    float step = (std::min)(io.DeltaTime, 0.1f) * 2.5f;
    s_Fade = s_Fade < fadeTo ? (std::min)(fadeTo, s_Fade + step) : (std::max)(fadeTo, s_Fade - step);
    float alpha = s_Opacity * 0.01f * s_Fade;
    if (!v.on || alpha < 0.01f) return;
    auto A = [alpha](ImU32 c) {
        ImU32 a = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xff) * alpha + 0.5f);
        return (c & ~IM_COL32_A_MASK) | a << IM_COL32_A_SHIFT;
    };
    float scale = io.DisplaySize.y / 1080.0f;
    float R = kRadius * scale, k = R / kRange;  // px per metre
    ImVec2 c(io.DisplaySize.x - kMarginX * scale - R, kMarginY * scale + R);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    std::vector<CellKey> wanted;
    {
        std::lock_guard<std::mutex> lock(s_TileLock);
        wanted = s_Wanted;
    }
    Upload(wanted);

    dl->AddCircleFilled(c, R, A(IM_COL32(28, 24, 18, 215)), 64);
    // world-map pixels -> screen (north up)
    auto toScreen = [&](float mx, float mz) { return ImVec2(c.x + (mx - v.mx) * k, c.y - (mz - v.mz) * k); };
    if (s_Atlas && v.map != NO_MAP) {
        std::vector<ImVec2> circle;
        for (int i = 0; i < 64; i++) {
            float a = i * 6.2831853f / 64;
            circle.push_back(ImVec2(c.x + cosf(a) * R, c.y + sinf(a) * R));
        }
        dl->PushTexture(ImTextureRef((ImTextureID)s_Atlas));
        ULONGLONG now = GetTickCount64();
        for (const CellKey& cell : wanted) {
            int slot = SlotOf(cell);
            if (slot < 0 || !s_Slots[slot].filled) continue;
            s_Slots[slot].used = now;
            // the cell on screen: x from 256 i to 256 i + 256, map Y up so its north edge is the top
            ImVec2 lo = toScreen((float)cell.x * kTile, (float)(cell.y + 1) * kTile);
            ImVec2 hi = toScreen((float)(cell.x + 1) * kTile, (float)cell.y * kTile);
            std::vector<ImVec2> poly = ClipToRect(circle, lo, hi);
            if (poly.size() < 3) continue;
            float u0 = (float)((slot % kSlotsPerRow) * kTile), v0 = (float)((slot / kSlotsPerRow) * kTile), atlas = (float)(kSlotsPerRow * kTile);
            int n = (int)poly.size();
            dl->PrimReserve((n - 2) * 3, n);
            ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
            for (const ImVec2& p : poly) {
                // screen -> position in the cell (image rows run southwards)
                float tx = (p.x - lo.x) / (hi.x - lo.x), ty = (p.y - lo.y) / (hi.y - lo.y);
                dl->PrimWriteVtx(p, ImVec2((u0 + tx * kTile) / atlas, (v0 + ty * kTile) / atlas), A(IM_COL32(255, 255, 255, 235)));
            }
            for (int i = 1; i + 1 < n; i++) {
                dl->PrimWriteIdx(base);
                dl->PrimWriteIdx((ImDrawIdx)(base + i));
                dl->PrimWriteIdx((ImDrawIdx)(base + i + 1));
            }
        }
        dl->PopTexture();
    }

    // rim and compass
    dl->AddCircle(c, R, A(IM_COL32(146, 139, 108, 230)), 64, 2.0f * scale);
    ImFont* f = font ? font : ImGui::GetFont();
    float fs = floorf(15.0f * scale);
    const char* dirs[] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) {
        float a = i * 1.5707963f;
        ImVec2 p(c.x + sinf(a) * (R - 9.0f * scale), c.y - cosf(a) * (R - 9.0f * scale));
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, dirs[i]);
        dl->AddText(f, fs, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), i == 0 ? A(IM_COL32(230, 200, 120, 255)) : A(IM_COL32(200, 190, 160, 220)), dirs[i]);
    }

    // marks: graces, beacons, the tracked spot, partners (on the rim when off the map)
    float lfs = floorf(13.0f * scale);
    for (int pass = 0; pass < 3; pass++) {
        for (const Mark& m : v.marks) {
            int group = m.kind == 2 ? 0 : m.kind >= 3 ? 1 : 2;
            if (group != pass) continue;
            ImVec2 p = toScreen(m.mx, m.mz);
            float dx = p.x - c.x, dy = p.y - c.y, d = sqrtf(dx * dx + dy * dy);
            bool clamped = d > R - 6.0f * scale;
            if (clamped) {
                if (m.kind == 2) continue;  // graces only inside
                p = ImVec2(c.x + dx / d * (R - 6.0f * scale), c.y + dy / d * (R - 6.0f * scale));
            }
            float r = 4.5f * scale;
            switch (m.kind) {
            case 2:  // grace
                dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), A(IM_COL32(240, 205, 110, 240)));
                break;
            case 3: {  // beacon
                dl->AddCircleFilled(p, r * 1.3f, A(IM_COL32(90, 170, 240, 240)));
                ImVec2 ts = f->CalcTextSizeA(lfs, FLT_MAX, 0.0f, m.label.c_str());
                dl->AddText(f, lfs, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), A(IM_COL32(10, 20, 30, 255)), m.label.c_str());
                break;
            }
            case 4:  // tracked (Find an item)
                dl->AddCircle(p, r * 1.6f, A(IM_COL32(240, 90, 70, 255)), 16, 2.0f * scale);
                dl->AddCircleFilled(p, r * 0.6f, A(IM_COL32(240, 90, 70, 255)));
                break;
            default: {  // partners
                ImU32 col = m.kind == 1 ? A(IM_COL32(120, 210, 120, 255)) : A(IM_COL32(110, 180, 250, 255));
                dl->AddCircleFilled(p, r * 1.2f, col);
                dl->AddCircle(p, r * 1.2f, A(IM_COL32(15, 15, 15, 220)), 16, 1.5f);
                if (!clamped) {
                    ImVec2 ts = f->CalcTextSizeA(lfs, FLT_MAX, 0.0f, m.label.c_str());
                    ImVec2 tp(p.x - ts.x * 0.5f, p.y + r * 1.6f);
                    dl->AddText(f, lfs, ImVec2(tp.x + 1, tp.y + 1), A(IM_COL32(0, 0, 0, 200)), m.label.c_str());
                    dl->AddText(f, lfs, tp, A(IM_COL32(235, 230, 215, 255)), m.label.c_str());
                }
            }
            }
        }
    }
    // you, facing where the camera looks
    DrawArrow(dl, c, v.haveHeading ? v.heading : 0.0f, 9.0f * scale, A(IM_COL32(245, 235, 200, 255)), A(IM_COL32(20, 18, 14, 230)));
}

} // namespace Minimap
