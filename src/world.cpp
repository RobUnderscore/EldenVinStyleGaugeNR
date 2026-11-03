#include "world.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace World {

// ---- game (2.7.2.0) ----

static uintptr_t s_Base = 0;
uintptr_t Base() { return s_Base; }
uintptr_t Global(uintptr_t rva) { return s_Base ? Ptr(s_Base + rva) : 0; }

static constexpr uintptr_t WORLD_CHR_MAN = 0x3d6d018;  // +0x1e508 the main player
static constexpr uintptr_t SOLO_PARAM_REPO = 0x3d88f78;
// MsgRepositoryImp, and its lookup (repo, 0, category, id) -> text or null; the game asks the newest
// DLC category first (item names: 4xx, 3xx, then the base one)
static constexpr uintptr_t MSG_REPO = 0x3d84588;
static constexpr uintptr_t MSG_LOOKUP = 0x2671370;
static const uint8_t kMsgLookupBytes[] = { 0x3b, 0x51, 0x10, 0x73, 0x29, 0x44, 0x3b, 0x41, 0x14, 0x73, 0x23 };
// CSFD4VirtualMemoryFlag and its getter (flags, id) -> on; it reads the world the game plays in, which for a
// Seamless Co-op guest is the host's
static constexpr uintptr_t FLAG_MAN = 0x3d6f4d8;
static constexpr uintptr_t GET_FLAG = 0x5fa700;
static const uint8_t kGetFlagBytes[] = { 0x44, 0x8b, 0x41, 0x1c, 0x44, 0x8b, 0xda, 0x33, 0xd2, 0x41, 0x8b, 0xc3, 0x41, 0xf7, 0xf0 };
// FD4Singleton<CSCamera> (a CSCameraImp, found by its vtable among the exe's statics) holds CSPersCams, whose
// matrix (+0x10, rows right/up/forward/position) places the camera
static constexpr uintptr_t CAMERA_VTABLE = 0x2a2d000;
static constexpr uintptr_t PERS_CAM_VTABLE = 0x2aa2b30;

static bool s_Checked = false, s_FlagOk = false, s_MsgOk = false;

void CheckCode() {
    if (s_Checked) return;
    s_Checked = true;
    s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!s_Base) return;
    const uint8_t* f = reinterpret_cast<const uint8_t*>(s_Base + GET_FLAG);
    const uint8_t* m = reinterpret_cast<const uint8_t*>(s_Base + MSG_LOOKUP);
    __try {
        // another mod's jump in front is fine: we only call them (2026-11-01 some mod in the pack hooks the getter)
        s_FlagOk = !memcmp(f, kGetFlagBytes, sizeof(kGetFlagBytes)) ||
            (f[0] == 0xE9 && !memcmp(f + 5, kGetFlagBytes + 5, sizeof(kGetFlagBytes) - 5));
        s_MsgOk = !memcmp(m, kMsgLookupBytes, sizeof(kMsgLookupBytes)) ||
            (m[0] == 0xE9 && !memcmp(m + 5, kMsgLookupBytes + 5, sizeof(kMsgLookupBytes) - 5));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log("DropFinder: flag getter %s%s, text lookup %s", s_FlagOk ? "ok" : "NOT as expected", s_FlagOk && f[0] == 0xE9 ? " (hooked by a mod)" : "",
        s_MsgOk ? "ok" : "NOT as expected");
}

// 1 on, 0 off, -1 can't tell
int Flag(uint32_t id) {
    if (!s_FlagOk || !id) return -1;
    uintptr_t man = Global(FLAG_MAN);
    if (!man) return -1;
    __try {
        return reinterpret_cast<bool (*)(uintptr_t, uint32_t)>(s_Base + GET_FLAG)(man, id) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const wchar_t* RawText(uint32_t category, int32_t id) {
    uintptr_t repo = Global(MSG_REPO);
    if (!s_MsgOk || !repo) return nullptr;
    __try {
        return reinterpret_cast<const wchar_t* (*)(uintptr_t, uint32_t, uint32_t, int32_t)>(s_Base + MSG_LOOKUP)(repo, 0, category, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string Utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}


std::string Text(const Cats& cats, int32_t id) {
    if (id <= 0) return {};
    for (uint32_t c : cats.c) {
        const wchar_t* t = RawText(c, id);
        if (t && *t && wcscmp(t, L"[ERROR]") && wcscmp(t, L"%null%")) return Utf8(t);
    }
    return {};
}

// ---- param tables ----
// SoloParamRepository: table i's ResCap at +0x88 + 0x48 i (name at +0x18), its param file at [[ResCap+0x80]+0x80]:
// row count u16 at +0xa, rows from +0x40, 0x18 bytes each {id, pad, data offset}

Table FindTable(const wchar_t* name) {
    uintptr_t repo = Global(SOLO_PARAM_REPO);
    if (!repo) return {};
    for (int slot = 0; slot < 300; slot++) {
        if (!Rd<uint32_t>(repo + 0x80 + slot * 0x48)) continue;
        uintptr_t cap = Ptr(repo + 0x88 + slot * 0x48);
        uintptr_t text = cap ? Ptr(cap + 0x18) : 0;
        if (!text) continue;
        bool same = true;
        for (size_t i = 0;; i++) {
            wchar_t c = Rd<wchar_t>(text + i * 2);
            if (c != name[i]) { same = false; break; }
            if (!c) break;
        }
        if (!same) continue;
        Table t;
        t.file = Ptr(Ptr(cap + 0x80) + 0x80);
        t.rows = t.file ? Rd<uint16_t>(t.file + 0xa) : 0;
        return t;
    }
    return {};
}

// Row layouts (from the paramdefs; row sizes checked against the regulation's files)
namespace Grace {  // BonfireWarpParam, 0xec
constexpr int FLAG = 0x04, AREA = 0x20, POS = 0x24, TEXT = 0x30;
}
namespace Conv {  // WorldMapLegacyConvParam, 0x30
constexpr int SRC = 0x04, SRC_POS = 0x08, DST = 0x14, DST_POS = 0x18;
}

// ---- where the game is ----

struct ConvRow {
    int sa, sx, sz, da, dx, dz;
    float sp[3], dp[3];
};
static std::vector<ConvRow> s_Conv;

uint32_t MapId(int area, int gx, int gz) { return (uint32_t)area << 24 | (uint32_t)gx << 16 | (uint32_t)gz << 8; }

// A position inside a map (block) on the world map's frame. m60/m61 tiles are 256 m (variant x1: 512 m, x2: 1024
// m, centred on their small tiles; the tens digit is an event variant). The game keeps the variant as nibbles
// (m60_45_36_10 is 0x10 in the player's block id) where DropFinder.txt has it in decimal (0x0a): both work. Other
// maps go through
// WorldMapLegacyConvParam, the anchor nearest the spot, rows leading straight to the overworld first (the
// Shunning-Grounds only reach it through Leyndell). Roundtable Hold has a row but no place on the map, so it keeps
// its own space. (The same rules as kubilaiswf-minimap's locate(), MIT, checked there against the game's map.)
static bool ToWorldAt(uint32_t map, float lx, float ly, float lz, WPos& out, int depth) {
    int area = map >> 24, gx = (map >> 16) & 0xff, gz = (map >> 8) & 0xff, kind = map & 0xf;
    if (kind > 2) kind = (map & 0xff) % 10;
    if (area == 60 || area == 61) {
        float size = 256.0f * (float)(1 << (kind > 2 ? 2 : kind)), off = (size - 256.0f) / 2;
        out = { area, gx * size + off + lx, ly, gz * size + off + lz };
        return true;
    }
    bool roundtable = area == 11 && gx == 10;
    if (depth < 4 && !roundtable) {
        for (int toWorld = 1; toWorld >= 0; toWorld--) {
            const ConvRow* best = nullptr;
            float bestD = 1e30f;
            for (const ConvRow& c : s_Conv) {
                if (c.sa != area || c.sx != gx || c.sz != gz || (c.da == area && c.dx == gx && c.dz == gz)) continue;
                if ((c.da == 60 || c.da == 61) != (toWorld == 1)) continue;
                float dx = lx - c.sp[0], dz = lz - c.sp[2], d = dx * dx + dz * dz;
                if (d < bestD) { bestD = d; best = &c; }
            }
            if (best)
                return ToWorldAt(MapId(best->da, best->dx, best->dz), lx - best->sp[0] + best->dp[0], ly - best->sp[1] + best->dp[1],
                               lz - best->sp[2] + best->dp[2], out, depth + 1);
        }
    }
    out = { (int)(map & 0xffffff00u), lx, ly, lz };
    return false;
}

bool ToWorld(uint32_t map, float lx, float ly, float lz, WPos& out) { return ToWorldAt(map, lx, ly, lz, out, 0); }

static std::vector<GraceInfo> s_Graces;  // task thread
static bool s_Loaded = false;

const std::vector<GraceInfo>& Graces() { return s_Graces; }

static bool LoadWorldTables() {
    Table conv = FindTable(L"WorldMapLegacyConvParam"), graces = FindTable(L"BonfireWarpParam");
    if (!conv.rows || !graces.rows) return false;
    s_Conv.clear();
    for (int i = 0; i < conv.rows; i++) {
        uintptr_t r = conv.Data(i);
        ConvRow c;
        c.sa = Rd<uint8_t>(r + Conv::SRC); c.sx = Rd<uint8_t>(r + Conv::SRC + 1); c.sz = Rd<uint8_t>(r + Conv::SRC + 2);
        c.da = Rd<uint8_t>(r + Conv::DST); c.dx = Rd<uint8_t>(r + Conv::DST + 1); c.dz = Rd<uint8_t>(r + Conv::DST + 2);
        for (int k = 0; k < 3; k++) {
            c.sp[k] = Rd<float>(r + Conv::SRC_POS + k * 4);
            c.dp[k] = Rd<float>(r + Conv::DST_POS + k * 4);
        }
        s_Conv.push_back(c);
    }
    s_Graces.clear();
    for (int i = 0; i < graces.rows; i++) {
        uintptr_t r = graces.Data(i);
        uint32_t flag = Rd<uint32_t>(r + Grace::FLAG);
        std::string name = Text(PLACE_NAME, Rd<int32_t>(r + Grace::TEXT));
        if (!flag || name.empty()) continue;
        GraceInfo g{ name, flag, MapId(Rd<uint8_t>(r + Grace::AREA), Rd<uint8_t>(r + Grace::AREA + 1), Rd<uint8_t>(r + Grace::AREA + 2)), {} };
        ToWorld(g.map, Rd<float>(r + Grace::POS), Rd<float>(r + Grace::POS + 4), Rd<float>(r + Grace::POS + 8), g.pos);
        s_Graces.push_back(g);
    }
    Log("DropFinder: %d map conversions, %d graces", (int)s_Conv.size(), (int)s_Graces.size());
    return !s_Graces.empty();
}

// [[chr+0x190]+0x68]+0x70
bool PhysicsPos(uintptr_t chr, float out[3]) {
    uintptr_t mods = Ptr(chr + 0x190);
    uintptr_t physics = mods ? Ptr(mods + 0x68) : 0;
    if (!physics) return false;
    for (int i = 0; i < 3; i++) {
        out[i] = Rd<float>(physics + 0x70 + i * 4);
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

uintptr_t MainPlayer() {
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    return wcm ? Ptr(wcm + 0x1e508) : 0;
}

// Where the main player stands (PlayerIns +0x6d0 block, +0x6c0 position in it) and their physics position
bool Player(WPos& at, float phys[3], uint32_t* blockOut) {
    uintptr_t main = MainPlayer();
    if (!main) return false;
    uint32_t block = Rd<uint32_t>(main + 0x6d0);
    if (blockOut) *blockOut = block;
    if (!block || block == 0xffffffffu) return false;
    float x = Rd<float>(main + 0x6c0), y = Rd<float>(main + 0x6c4), z = Rd<float>(main + 0x6c8);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    ToWorld(block, x, y, z, at);
    return PhysicsPos(main, phys);
}

static uintptr_t s_CamStatic = 0;
static ULONGLONG s_CamSearchAt = 0;
static int s_CamSearches = 0;

static uintptr_t Camera() {
    if (s_CamStatic) {
        uintptr_t cam = Ptr(s_CamStatic);
        if (cam && Rd<uintptr_t>(cam) == s_Base + CAMERA_VTABLE) return cam;
        s_CamStatic = 0;
    }
    ULONGLONG now = GetTickCount64();
    if (now < s_CamSearchAt) return 0;
    s_CamSearchAt = now + (++s_CamSearches < 5 ? 5000 : 60000);
    // the game's other statics sit around 0x3d6xxxx
    for (uintptr_t rva = 0x3c00000; rva < 0x4000000; rva += 8) {
        uintptr_t p = Rd<uintptr_t>(s_Base + rva);
        if (!Valid(p) || (p >= s_Base && p < s_Base + 0x6000000)) continue;
        if (Rd<uintptr_t>(p) == s_Base + CAMERA_VTABLE) {
            s_CamStatic = s_Base + rva;
            Log("DropFinder: camera at static +0x%llX", (unsigned long long)rva);
            return p;
        }
    }
    return 0;
}

// The direction the camera looks (degrees, 0 = north, 90 = east): from the camera to the player it follows,
// or its own forward row when it sits on the player
bool CameraHeading(const float player[3], float& heading) {
    uintptr_t cam = Camera();
    if (!cam) return false;
    for (int off = 0x8; off <= 0x40; off += 8) {
        uintptr_t pc = Ptr(cam + off);
        if (!pc || Rd<uintptr_t>(pc) != s_Base + PERS_CAM_VTABLE) continue;
        float fx = Rd<float>(pc + 0x30), fy = Rd<float>(pc + 0x34), fz = Rd<float>(pc + 0x38);
        float cx = Rd<float>(pc + 0x40), cz = Rd<float>(pc + 0x48);
        float len = sqrtf(fx * fx + fy * fy + fz * fz);
        if (!std::isfinite(len) || len < 0.9f || len > 1.1f) continue;
        float hx = player[0] - cx, hz = player[2] - cz;
        float d = sqrtf(hx * hx + hz * hz);
        if (d > 0.5f && d < 40.0f) heading = atan2f(hx, hz) * 57.29578f;
        else heading = atan2f(fx, fz) * 57.29578f;
        return std::isfinite(heading);
    }
    return false;
}

bool Ready() {
    CheckCode();
    if (s_Loaded) return true;
    if (!Global(SOLO_PARAM_REPO) || !Global(MSG_REPO)) return false;
    s_Loaded = LoadWorldTables();
    return s_Loaded;
}

} // namespace World
