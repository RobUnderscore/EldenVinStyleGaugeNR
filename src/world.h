#pragma once
#include "memory.h"
#include <cstdint>
#include <string>
#include <vector>

// Elden Ring (2.7.2.0) game data shared by Find an item, the minimap and the target panel: game text, event flags,
// param tables, positions on the world map (overworld frame), graces, the main player and the camera. Everything
// here is called from the game's task thread (CoopResync's per-frame task).
namespace World {

template <typename T> T Rd(uintptr_t a) { return Memory::Read<T>(a); }
inline bool Valid(uintptr_t p) { return p >= 0x10000 && p < 0x800000000000ull; }
inline uintptr_t Ptr(uintptr_t a) {
    uintptr_t p = Rd<uintptr_t>(a);
    return Valid(p) ? p : 0;
}

uintptr_t Base();                 // eldenring.exe, 0 when this isn't Elden Ring
uintptr_t Global(uintptr_t rva);  // the pointer a static at this RVA holds (0 when unset)

// Check the game functions we call (once); then Flag and Text work
void CheckCode();
// Ready to use: param tables and text loaded, world-map conversions and graces read (false until then)
bool Ready();

// 1 on, 0 off, -1 can't tell
int Flag(uint32_t id);

// Game text, the newest DLC category first, as UTF-8 ("" for none)
struct Cats { uint32_t c[3]; };
constexpr Cats GOODS_NAME{ { 419, 319, 10 } }, WEAPON_NAME{ { 410, 310, 11 } }, PROTECTOR_NAME{ { 413, 313, 12 } },
    ACCESSORY_NAME{ { 416, 316, 13 } }, GEM_NAME{ { 422, 322, 35 } }, NPC_NAME{ { 428, 328, 18 } }, PLACE_NAME{ { 429, 329, 19 } };
// An item's one-line summary (Info) and its description (Caption); weapons have no summary
constexpr Cats GOODS_INFO{ { 420, 320, 20 } }, WEAPON_INFO{ { 411, 311, 21 } }, PROTECTOR_INFO{ { 414, 314, 22 } },
    ACCESSORY_INFO{ { 417, 317, 23 } }, GEM_INFO{ { 423, 323, 36 } };
constexpr Cats GOODS_CAPTION{ { 421, 321, 24 } }, WEAPON_CAPTION{ { 412, 312, 25 } }, PROTECTOR_CAPTION{ { 415, 315, 26 } },
    ACCESSORY_CAPTION{ { 418, 318, 27 } }, GEM_CAPTION{ { 424, 324, 37 } };
std::string Text(const Cats& cats, int32_t id);
std::string Utf8(const wchar_t* w);

// A param table: SoloParamRepository's file, row count u16 at +0xa, rows from +0x40 (0x18 bytes: id, pad, offset)
struct Table {
    uintptr_t file = 0;
    int rows = 0;
    int Id(int i) const { return Rd<int32_t>(file + 0x40 + i * 0x18); }
    uintptr_t Data(int i) const { return file + Rd<int64_t>(file + 0x40 + i * 0x18 + 8); }
    int Find(int32_t id) const {
        int lo = 0, hi = rows - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, v = Id(mid);
            if (v == id) return mid;
            if (v < id) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }
    uintptr_t Row(int32_t id) const {
        int i = Find(id);
        return i < 0 ? 0 : Data(i);
    }
};
Table FindTable(const wchar_t* name);

// The overworld frame (60, 61: X east, Z north, metres; 256 m tiles), or a map no table places on it (its id)
struct WPos {
    int space = 0;
    float x = 0, y = 0, z = 0;
};
uint32_t MapId(int area, int gx, int gz);
// A position inside a map (block id as PlayerIns+0x6d0 keeps it) on that frame; false when it stays in its own
bool ToWorld(uint32_t map, float lx, float ly, float lz, WPos& out);

struct GraceInfo {
    std::string name;
    uint32_t flag;
    uint32_t map;  // the map it's placed in
    WPos pos;
};
const std::vector<GraceInfo>& Graces();

uintptr_t MainPlayer();
bool PhysicsPos(uintptr_t chr, float out[3]);  // [[chr+0x190]+0x68]+0x70
// Where the main player stands and their physics position (and the block id it's relative to)
bool Player(WPos& at, float phys[3], uint32_t* block = nullptr);
// The direction the camera looks (degrees, 0 = north, 90 = east)
bool CameraHeading(const float player[3], float& heading);

} // namespace World
