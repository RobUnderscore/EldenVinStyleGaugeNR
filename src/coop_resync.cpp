#include "coop_resync.h"
#include "config.h"
#include "convenience_menu.h"
#include "drop_finder.h"
#include "minimap.h"
#include "target.h"
#include "hud_icons.h"
#include "flying_mount.h"
#include "horse_up.h"
#include "world.h"
#include "native_menu.h"
#include "game_textures.h"
#include "imgui.h"
#include "memory.h"
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

// How another player's character works here (eldenring.exe 2.7.2.0, found on the er-coop rig
// and in the Seamless logs; offsets also in fromsoftware-rs):
// - Each other player is a "copy" in WorldChrMan's player ChrSet, created by the game for a
//   session member whose join_wait (+0xea) is set. A healthy copy goes status 3 (waiting for
//   network data) -> 4 with update type 4 (network driven) and fades in.
// - Stuck copy 1, after a death: the death animation fades it to opacity 0 and the activation
//   check switches it to update type 2 for good. Fixed by the PlayerIns respawn reset (vf180).
// - Stuck copy 2, rebuilt at a bad moment (their death/reload): it takes the local path
//   (status 1/2, type 2) or sits in 4/4 with its base transparency stuck at 0, hundreds of
//   metres off. Fixed by removing it the way the game does when a member leaves, then setting
//   join_wait so the game builds it again as on join.
// - Not signs of a bad copy: a new copy starts at alpha 0 and fades in over about a second, and
//   its block origin (+0x3c) stays unset until the other player first moves (13 s on the rig).
// - Never rebuild while they're dead or far away: the new copy fails the activation check on its
//   first frame and takes the local path, i.e. the rebuild itself makes stuck copy 2 (seen on the
//   rig). A switched-off copy still gets their position from the network, so its distance is real.
namespace CoopResync {

// ---- game addresses (RVAs, 2.7.2.0) ----

static constexpr uintptr_t WORLD_CHR_MAN = 0x3d6d018;
static constexpr uintptr_t GAME_DATA_MAN = 0x3d64fb8;
static constexpr uintptr_t GAME_MAN = 0x3d709a8;
static constexpr uintptr_t SESSION_MAN = 0x3d81560;
static constexpr uintptr_t CS_TASK = 0x4592ea8;
// CS::PlayerNetworkSession's vtable; its +0x48 returns this+8, where the remote Steam ID sits
static constexpr uintptr_t NET_SESSION_VTABLE = 0x2ba3ed0;

struct Site {
    const char* name;
    uintptr_t rva;
    const uint8_t* bytes;
    size_t len;
};

#define SITE(var, label, rva, ...) \
    static const uint8_t var##_BYTES[] = { __VA_ARGS__ }; \
    static const Site var = { label, rva, var##_BYTES, sizeof(var##_BYTES) }

// (CSTaskImp, group, task): add a recurring task; group 0 = FrameBegin
SITE(REGISTER_TASK, "register task", 0xeb5540, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d, 0x4c, 0x24, 0x20, 0x49);
// (): start a map reload to GameMan's destination (fast travel, respawns, warps all start here)
SITE(MAP_RELOAD, "map reload", 0x5f8e70, 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0xb1, 0x01);
// (out, &block) and (&pos4, &rot4): the next reload's destination, as the game's warp-to-host sets it
SITE(SET_RELOAD_BLOCK, "reload block setter", 0x67bed0, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x02, 0x48, 0x8b, 0xd9);
SITE(SET_RELOAD_POS, "reload position setter", 0x67be20, 0x0f, 0x28, 0x01, 0x48, 0x8b, 0x05);
// (CSLuaEventMan, leaving PlayerGameData): "has left" message; for the host it pulls a guest home
SITE(MEMBER_LEFT, "member-left handler", 0x58e880, 0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48);
// (PartyMemberInfo, handle, member+0xdc): remove a player's party slot (calls the member-left handler)
SITE(REMOVE_SLOT, "slot removal", 0x9fbd00, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48);
// (WorldChrMan, chr): take a character out of its ChrSet and queue its deletion
SITE(REMOVE_CHR, "remove chr", 0x50b5c0, 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x29, 0x02, 0x00, 0x00, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7);
// PlayerIns vtable +0x5a0 (chr, &pos4, &rot4, 0): respawn reset (death state off, opacity 1, full HP)
SITE(VF180, "PlayerIns respawn reset", 0x658e60, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57);
// PlayerIns vtable +0x530 (chr): called on a copy before it is removed
SITE(BEFORE_REMOVE, "PlayerIns before-removal", 0x655d30, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01, 0x48, 0x8b, 0xd9, 0xff, 0x90, 0x20, 0x01);
// mov rax,[GameMan]; mov rax,[rax+0xd90]; ret  (PartyMemberInfo)
SITE(PMI_GETTER, "PartyMemberInfo getter", 0x67b5d0, 0x48, 0x8b, 0x05, 0xd1, 0x53, 0x6f, 0x03, 0x48, 0x8b, 0x80, 0x90, 0x0d, 0x00, 0x00, 0xc3, 0xcc);
// lea rax,[rcx+8]; ret  (PlayerNetworkSession vtable +0x48)
SITE(NET_ID_GETTER, "network session id getter", 0xcc81b0, 0x48, 0x8d, 0x41, 0x08, 0xc3);
// The helpers 0x65b1b0 uses to keep the main player's block and in-block position (PlayerIns
// +0x6d0/+0x6c0, what a reload takes); it only does that for the main player, we use them on the
// host's character:
// (chr, &block): the block its physics position is relative to (+0x38, else its origin +0x3c)
SITE(REF_BLOCK, "chr reference block", 0x3f0490, 0xc7, 0x02, 0xff, 0xff, 0xff, 0xff, 0x8b, 0x41, 0x38, 0x83, 0xf8, 0xff, 0x75, 0x03, 0x8b, 0x41);
// (chr, &buf) -> its physics position
SITE(PHYS_POS, "chr physics position", 0x3f1a40, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xd9);
// (&pos out, &pos, &block in/out) -> bool: the position relative to the block it is in
SITE(TO_BLOCK_POS, "position to block position", 0x61f570, 0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x05, 0x9d);
// (chr, &buf) -> its rotation (yaw at +4)
SITE(ROTATION, "chr rotation", 0x3eebf0, 0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x05, 0x1f, 0x32, 0x87);
// (&out, &block) -> the block id in the form PlayerIns+0x6d0 keeps
SITE(NORM_BLOCK, "block id normaliser", 0x6620c0, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xc7, 0x01, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x8b, 0xca, 0x44);
// The host spot the game's own warp-to-host (0x5f3950) uses: BreakInManager (CSNetMan+0xa8) ->
// +0x98 -> +0xa8 block, +0xac x, y, +0xb4 z, +0xb8 yaw. Recorded by the join flow, may be old.
SITE(HOST_SPOT_GETTER, "host spot getter", 0xa05170, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x89, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xda);
SITE(HOST_SPOT_READER, "host spot reader", 0xa05190, 0x8b, 0x81, 0xa8, 0x00, 0x00, 0x00, 0x89, 0x02, 0xf2, 0x0f, 0x10, 0x81, 0xac, 0x00, 0x00, 0x00);
static constexpr uintptr_t CS_NET_MAN = 0x3d61ee0;

static uintptr_t s_Base = 0;
static uintptr_t Addr(const Site& s) { return s_Base + s.rva; }

// The original bytes, or (allowHook, for functions we only call) another mod's jump in front of them
static bool Intact(const Site& s, bool allowHook) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(Addr(s));
    __try {
        if (memcmp(p, s.bytes, s.len) == 0) return true;
        return allowHook && (p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What can be used (checked in Init, logged)
static bool s_NetIdOk = false, s_ReviveOk = false, s_RebuildOk = false, s_ReloadOk = false;
static bool s_SpotOk = false, s_HostSpotOk = false;
static bool s_GuardHooked = false;

// ---- memory ----

template <typename T> static T Rd(uintptr_t a) { return Memory::Read<T>(a); }
static bool Valid(uintptr_t p) { return p >= 0x10000 && p < 0x800000000000ull; }
static uintptr_t Ptr(uintptr_t a) {
    uintptr_t p = Rd<uintptr_t>(a);
    return Valid(p) ? p : 0;
}
static uintptr_t Global(uintptr_t rva) { return Ptr(s_Base + rva); }

// Param tables: SoloParamRepository [0x3d88f78]; table i's ResCap at +0x88 + 0x48 i (name at +0x18), its param
// file at [[ResCap+0x80]+0x80]: row count u16 at +0xa, rows from +0x40, 0x18 bytes each {id, pad, data offset}
static constexpr uintptr_t SOLO_PARAM_REPO = 0x3d88f78;

// The param file in table `slot` if that table is `name` (0 when it isn't, or isn't loaded yet)
static uintptr_t ParamFile(int slot, const wchar_t* name) {
    uintptr_t repo = Global(SOLO_PARAM_REPO);
    if (!repo || !Rd<uint32_t>(repo + 0x80 + slot * 0x48)) return 0;
    uintptr_t cap = Ptr(repo + 0x88 + slot * 0x48);
    uintptr_t text = cap ? Ptr(cap + 0x18) : 0;
    if (!text) return 0;
    for (size_t i = 0;; i++) {
        wchar_t c = Rd<wchar_t>(text + i * 2);
        if (c != name[i]) return 0;
        if (!c) break;
    }
    return Ptr(Ptr(cap + 0x80) + 0x80);
}

// ---- session ----

struct Member {
    uintptr_t entry;
    uint64_t steamId;
    int32_t gameDataIndex;  // remote PlayerGameData slot, -1 for the host
    bool isHost, isLocal;
};

// CSSessionManager+0xc: 3 host, 6 guest
static int LobbyState() {
    uintptr_t sm = Global(SESSION_MAN);
    return sm ? Rd<int32_t>(sm + 0xc) : 0;
}
static bool InSession(int lobby) { return lobby == 3 || lobby == 6; }

// CSSessionManager+0x70: the player list (allocator, begin, end, capacity) of 0x100-byte entries
static std::vector<Member> Members() {
    std::vector<Member> out;
    uintptr_t sm = Global(SESSION_MAN);
    if (!sm) return out;
    uintptr_t begin = Rd<uintptr_t>(sm + 0x78), end = Rd<uintptr_t>(sm + 0x80);
    if (!Valid(begin) || end < begin || end - begin > 0x100 * 8) return out;
    for (uintptr_t e = begin; e + 0x100 <= end; e += 0x100)
        out.push_back({ e, Rd<uint64_t>(e + 0x10), Rd<int32_t>(e + 0xd0), Rd<uint8_t>(e + 0xe8) != 0, Rd<uint8_t>(e + 0xe9) != 0 });
    return out;
}

// ---- characters ----

static uintptr_t MainPlayer() {
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    return wcm ? Ptr(wcm + 0x1e508) : 0;
}

// A ChrSetEntry: ChrIns*, status (1 initializing, 2 active local, 3 waiting for network data,
// 4 ready), update type (2 local, 4 network)
struct Copy {
    uintptr_t chr;
    uint8_t status, update;
};

// WorldChrMan+0x10ee0: the player ChrSet (capacity u32 at +0x10, 16-byte entries at [+0x18])
static std::vector<Copy> PlayerSet() {
    std::vector<Copy> out;
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    if (!wcm) return out;
    uint32_t cap = Rd<uint32_t>(wcm + 0x10ee0 + 0x10);
    uintptr_t entries = Ptr(wcm + 0x10ee0 + 0x18);
    if (!entries) return out;
    for (uint32_t i = 0; i < cap && i < 64; i++) {
        uintptr_t e = entries + i * 16;
        uintptr_t chr = Ptr(e);
        if (chr) out.push_back({ chr, Rd<uint8_t>(e + 8), Rd<uint8_t>(e + 9) });
    }
    return out;
}

// The Steam ID behind a copy, from its network session (PlayerIns+0x5b0); 0 while that is still
// being set up (base-class vtable, never called) or for our own character
static uint64_t SteamIdOf(uintptr_t chr) {
    if (!s_NetIdOk) return 0;
    uintptr_t net = Ptr(chr + 0x5b0);
    if (!net || Rd<uintptr_t>(net) != s_Base + NET_SESSION_VTABLE) return 0;
    return Rd<uint64_t>(net + 8);
}

// Remote PlayerGameData slot i: [GameDataMan+0x10] + i*0xaf0
static uintptr_t RemotePgd(int32_t i) {
    if (i < 0 || i >= 5) return 0;
    uintptr_t gdm = Global(GAME_DATA_MAN);
    uintptr_t list = gdm ? Ptr(gdm + 0x10) : 0;
    return list ? list + i * 0xaf0 : 0;
}

// The session member a copy belongs to: by its network session, else by its PlayerGameData
// (PlayerIns+0x580) being that member's remote slot
static const Member* OwnerOf(uintptr_t chr, const std::vector<Member>& members) {
    uint64_t id = SteamIdOf(chr);
    uintptr_t pgd = Ptr(chr + 0x580);
    for (const Member& m : members) {
        if (m.isLocal) continue;
        if (id ? m.steamId == id : (pgd && RemotePgd(m.gameDataIndex) == pgd)) return &m;
    }
    return nullptr;
}

// ChrCtrl(+0x58) -> modifier(+0xc8) +0x24 bit0
static bool IsDead(uintptr_t chr) {
    uintptr_t ctrl = Ptr(chr + 0x58);
    uintptr_t mod = ctrl ? Ptr(ctrl + 0xc8) : 0;
    return mod && (Rd<uint32_t>(mod + 0x24) & 1);
}

// The death fade (ChrIns+0x238)
static float FadeOpacity(uintptr_t chr) { return Rd<float>(chr + 0x238); }

// The lower of the death fade and the base transparency (+0x24c, stuck at 0 on a badly built
// copy); tint and camouflage (+0x240/+0x248) are left out, invisibility spells use them
static float Alpha(uintptr_t chr) { return fminf(FadeOpacity(chr), Rd<float>(chr + 0x24c)); }

// [[chr+0x190]+0x68]+0x70
static bool PhysicsPos(uintptr_t chr, float out[3]) {
    uintptr_t mods = Ptr(chr + 0x190);
    uintptr_t physics = mods ? Ptr(mods + 0x68) : 0;
    if (!physics) return false;
    for (int i = 0; i < 3; i++) {
        out[i] = Rd<float>(physics + 0x70 + i * 4);
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

// Metres between two characters in the same block (ChrIns+0x30: physics positions share its
// frame); -1 when the blocks differ or one is unset
static float Metres(uintptr_t a, uintptr_t b) {
    uint32_t ba = Rd<uint32_t>(a + 0x30), bb = Rd<uint32_t>(b + 0x30);
    float pa[3], pb[3];
    if (ba != bb || ba == 0xffffffff || !PhysicsPos(a, pa) || !PhysicsPos(b, pb)) return -1.0f;
    float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ChrDataModule ([[chr+0x190]]) +0x138
static int32_t Hp(uintptr_t chr) {
    uintptr_t mods = Ptr(chr + 0x190);
    uintptr_t data = mods ? Ptr(mods) : 0;
    return data ? Rd<int32_t>(data + 0x138) : 0;
}

// ---- names ----

static std::string Utf8(const wchar_t* w) {
    char buf[96];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 1 ? std::string(buf, n - 1) : std::string();
}

// PlayerGameData+0x9c: character name, 16 UTF-16 chars
static std::string PgdName(uintptr_t pgd) {
    if (!pgd) return {};
    wchar_t w[17] = {};
    for (int i = 0; i < 16; i++) w[i] = Rd<wchar_t>(pgd + 0x9c + i * 2);
    return Utf8(w);
}

// The Steam name in the member entry (an in-place UTF-16 string somewhere in +0x18..+0xc0)
static std::string SteamName(uintptr_t entry) {
    for (uintptr_t o = 0x18; o + 4 <= 0xc0; o += 2) {
        wchar_t w[64] = {};
        int n = 0;
        while (n < 63 && o + n * 2 < 0xc0) {
            wchar_t c = Rd<wchar_t>(entry + o + n * 2);
            if (c < 0x20 || c > 0x7e) break;  // printable ASCII only: pointers around it aren't
            w[n++] = c;
        }
        if (n >= 2) return Utf8(w);
    }
    return {};
}

static std::string NameOf(const Member& m, const Copy* c) {
    std::string name = c ? PgdName(Ptr(c->chr + 0x580)) : std::string();
    if (name.empty()) name = PgdName(RemotePgd(m.gameDataIndex));
    if (name.empty()) name = SteamName(m.entry);
    if (name.empty()) name = m.isHost ? "Host" : "Player";
    return name;
}

// ---- classifying a copy ----

enum class State { NoCharacter, Loading, Visible, FarAway, NotLoaded, JustDied, Hidden, Faded, BuiltWrong };

// When we first saw each copy, and since when it has been dead (task thread only)
struct Seen {
    uintptr_t chr;
    ULONGLONG at;
    ULONGLONG deadSince;  // 0 = alive
};
static std::vector<Seen> s_Seen;
static constexpr ULONGLONG kYoungMs = 5000;
// a death, the "YOU DIED" screen and the respawn load took about 11 s on the rig
static constexpr ULONGLONG kRespawnMs = 15000;
// switched off further than this (or in another block): far away, not stuck
static constexpr float kNearMetres = 150.0f;

static void TrackCopies(const std::vector<Copy>& set, ULONGLONG now) {
    std::vector<Seen> kept;
    for (const Copy& c : set) {
        Seen n = { c.chr, now, 0 };
        for (const Seen& s : s_Seen)
            if (s.chr == c.chr) { n = s; break; }
        n.deadSince = IsDead(c.chr) ? (n.deadSince ? n.deadSince : now) : 0;
        kept.push_back(n);
    }
    s_Seen = std::move(kept);
}

static const Seen* SeenOf(uintptr_t chr) {
    for (const Seen& s : s_Seen)
        if (s.chr == chr) return &s;
    return nullptr;
}

static void ForgetCopy(uintptr_t chr) {
    for (size_t i = 0; i < s_Seen.size(); i++)
        if (s_Seen[i].chr == chr) { s_Seen.erase(s_Seen.begin() + i); return; }
}

// `metres` gets the distance to us when known (-1 otherwise)
static State Classify(const Copy& c, uintptr_t main, ULONGLONG now, float* metres = nullptr) {
    const Seen* seen = SeenOf(c.chr);
    float d = Metres(main, c.chr);
    if (metres) *metres = d;
    // new: waiting for network data, fading in
    if (c.status == 3 || !seen || now - seen->at < kYoungMs) return State::Loading;
    bool dead = IsDead(c.chr);
    if (dead && now - seen->deadSince < kRespawnMs) return State::JustDied;
    // local path: Seamless builds them this way after a reload while they're far off, and such a
    // copy turned normal on its own once the players met (rig); a rebuild when together fixes it
    if ((c.status == 1 || c.status == 2) && c.update == 2) return State::NotLoaded;
    if (c.status == 4 && c.update == 4) {
        if (dead) return State::Faded;
        if (Alpha(c.chr) <= 0.0f) return State::BuiltWrong;
        return State::Visible;
    }
    if (c.status == 4 && c.update == 2) {
        if (dead || FadeOpacity(c.chr) <= 0.0f) return State::Faded;
        if (d < 0.0f || d > kNearMetres) return State::FarAway;
        return State::Hidden;  // switched off right next to us: stuck
    }
    return State::Loading;
}

static const char* StateLabel(State s) {
    switch (s) {
    case State::NoCharacter: return "no character";
    case State::Loading: return "loading in";
    case State::Visible: return "visible";
    case State::FarAway: return "far away";
    case State::NotLoaded: return "not loaded (far away?)";
    case State::JustDied: return "down, respawning";
    case State::Hidden: return "hidden (stuck?)";
    case State::Faded: return "stuck after a death";
    case State::BuiltWrong: return "stuck (loaded wrong)";
    }
    return "?";
}

// ---- what the menu sees ----

struct MemberView {
    uint64_t steamId;
    std::string name;
    bool isHost;
    State state;
    int metres;  // -1 unknown
};

static std::mutex s_Lock;
static std::vector<MemberView> s_View;  // other players
static int s_ViewLobby = 0;
static bool s_ViewInGame = false;
static bool s_ViewBoss = false;  // the game's boss fight flag (GameDataMan+0xc0) is on
static std::string s_Message;
static ULONGLONG s_MessageAt = 0;

static std::atomic<bool> s_TaskRegistered{ false }, s_TaskFailed{ false }, s_TaskSeen{ false }, s_Faulted{ false };
static std::atomic<bool> s_WantResync{ false }, s_WantReload{ false };
static std::atomic<uint64_t> s_WantGoTo{ 0 };  // Go to: that player's Steam ID
static std::atomic<bool> s_WantClearBoss{ false };

static uint32_t s_MessageSerial = 0;  // under s_Lock

static void SetMessage(const std::string& msg) {
    Log("CoopResync: %s", msg.c_str());
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Message = msg;
    s_MessageAt = GetTickCount64();
    s_MessageSerial++;
}

static void Snapshot() {
    int lobby = LobbyState();
    std::vector<MemberView> view;
    uintptr_t main = MainPlayer();
    if (InSession(lobby) && main) {
        std::vector<Member> members = Members();
        std::vector<Copy> set = PlayerSet();
        ULONGLONG now = GetTickCount64();
        TrackCopies(set, now);
        for (const Member& m : members) {
            if (m.isLocal) continue;
            const Copy* mine = nullptr;
            for (const Copy& c : set)
                if (c.chr != main && OwnerOf(c.chr, members) == &m) { mine = &c; break; }
            float d = -1.0f;
            State st = mine ? Classify(*mine, main, now, &d) : State::NoCharacter;
            view.push_back({ m.steamId, NameOf(m, mine), m.isHost, st, (int)d });
        }
    }
    uintptr_t gdm = Global(GAME_DATA_MAN);
    bool boss = main && gdm && Rd<uint8_t>(gdm + 0xc0);
    std::lock_guard<std::mutex> lock(s_Lock);
    s_View = std::move(view);
    s_ViewLobby = main ? lobby : 0;
    s_ViewInGame = main != 0;
    s_ViewBoss = boss;
}

// ---- repairs (game task thread) ----

using MemberLeftFn = void (*)(void* luaEventMan, void* leavingPgd);
static MemberLeftFn s_OrigMemberLeft = nullptr;
// the thread removing a copy right now (its member-left call is skipped), 0 = none
static std::atomic<DWORD> s_RebuildThread{ 0 };

static void HkMemberLeft(void* luaEventMan, void* leavingPgd) {
    if (s_RebuildThread.load() == GetCurrentThreadId()) {
        Log("CoopResync: member-left handler during our rebuild: skipped (no \"has left\", no pull-back)");
        return;
    }
    s_OrigMemberLeft(luaEventMan, leavingPgd);
}

// Our jump is still at the handler (a mod or the game's guards could have put the bytes back)
static bool GuardActive() { return s_GuardHooked && Rd<uint8_t>(Addr(MEMBER_LEFT)) == 0xE9; }

// PlayerIns respawn reset at the copy's position (the network moves it from there)
static bool Revive(uintptr_t chr) {
    uintptr_t vt = Ptr(chr);
    float p[3];
    if (!s_ReviveOk || !vt || Rd<uintptr_t>(vt + 0x5a0) != Addr(VF180) || !PhysicsPos(chr, p)) return false;
    alignas(16) float pos[4] = { p[0], p[1], p[2], 1.0f };
    alignas(16) float rot[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    reinterpret_cast<uint64_t (*)(uintptr_t, const float*, const float*, uint8_t)>(Addr(VF180))(chr, pos, rot, 0);
    Log("CoopResync: revived copy %llX at (%.1f, %.1f, %.1f)", (unsigned long long)chr, p[0], p[1], p[2]);
    return true;
}

// Removes the copy the way the game handles a member leaving (party slot, +0x530, ChrSet) with
// the member-left handler skipped, then sets the member's join_wait so the game builds them again.
// Empty result = done, else why not.
static std::string Rebuild(const Member& m, uintptr_t chr, int lobby) {
    uintptr_t gm = Global(GAME_MAN), wcm = Global(WORLD_CHR_MAN);
    uintptr_t pmi = gm ? Ptr(gm + 0xd90) : 0;
    if (!s_RebuildOk || !pmi || !wcm || !Intact(REMOVE_SLOT, true) || !Intact(REMOVE_CHR, true)) return "can't rebuild here";
    // removing the host's slot on a guest without the guard sends the guest home
    if (lobby == 6 && m.isHost && !GuardActive()) return "can't rebuild the host's character safely, try Reload here";
    uintptr_t vt = Ptr(chr);
    if (!vt || Rd<uintptr_t>(vt + 0x530) != Addr(BEFORE_REMOVE)) return "not a player character";

    uint64_t handle = Rd<uint64_t>(chr + 8);
    uint32_t slot = Rd<uint32_t>(m.entry + 0xdc);
    Log("CoopResync: rebuilding %llX's copy %llX (handle %llX, slot %X)", (unsigned long long)m.steamId,
        (unsigned long long)chr, (unsigned long long)handle, slot);
    auto removeSlot = reinterpret_cast<void (*)(uintptr_t, uint64_t, uint32_t)>(Addr(REMOVE_SLOT));
    auto beforeRemove = reinterpret_cast<void (*)(uintptr_t)>(Addr(BEFORE_REMOVE));
    auto removeChr = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(Addr(REMOVE_CHR));
    s_RebuildThread = GetCurrentThreadId();
    removeSlot(pmi, handle, slot);
    beforeRemove(chr);
    removeChr(wcm, chr);
    s_RebuildThread = 0;
    ForgetCopy(chr);
    *reinterpret_cast<volatile uint8_t*>(m.entry + 0xea) = 1;
    return {};
}

struct Revived {
    uint64_t steamId;
    std::string name;
    bool mayRebuild;  // false: revived by the automatic resync in a boss fight
};
static std::vector<Revived> s_Revived;
static ULONGLONG s_FollowUpAt = 0;

static const Copy* CopyOf(const Member& m, const std::vector<Member>& members, const std::vector<Copy>& set, uintptr_t main) {
    for (const Copy& c : set)
        if (c.chr != main && OwnerOf(c.chr, members) == &m) return &c;
    return nullptr;
}

// The repair for a copy in state `st` (one that needs one); returns the line for the report.
// `unmatched`: copies we couldn't put a member to (then a missing one isn't built again)
static std::string Repair(const Member& m, const Copy* c, State st, int lobby, int unmatched, const std::string& name,
                          bool mayRebuild) {
    switch (st) {
    case State::NoCharacter:
        // a copy we couldn't match may be theirs: building a second one would be worse
        if (unmatched) return "no character found, try Reload here";
        *reinterpret_cast<volatile uint8_t*>(m.entry + 0xea) = 1;
        Log("CoopResync: %llX had no copy: join_wait set", (unsigned long long)m.steamId);
        return "loading their character";
    case State::Faded:
        if (Revive(c->chr)) {
            s_Revived.push_back({ m.steamId, name, mayRebuild });
            return "brought back";
        }
        [[fallthrough]];
    case State::Hidden:
    case State::NotLoaded:
    case State::BuiltWrong: {
        if (!mayRebuild) return "left alone in a boss fight";
        std::string why = Rebuild(m, c->chr, lobby);
        return why.empty() ? "rebuilding their character" : why;
    }
    default:
        return "fine";
    }
}

static void StartResync(ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    if (!InSession(lobby)) return SetMessage("Not in a co-op session");
    if (!main) return SetMessage("Wait until you're loaded in");

    std::vector<Member> members = Members();
    std::vector<Copy> set = PlayerSet();
    TrackCopies(set, now);
    int unmatched = 0;
    for (const Copy& c : set)
        if (c.chr != main && !OwnerOf(c.chr, members)) unmatched++;

    std::string report;
    s_Revived.clear();
    for (const Member& m : members) {
        if (m.isLocal) continue;
        const Copy* c = CopyOf(m, members, set, main);
        std::string name = NameOf(m, c);
        State st = c ? Classify(*c, main, now) : State::NoCharacter;
        std::string line;
        switch (st) {
        case State::Visible: line = "already visible"; break;
        case State::Loading: line = "still loading in, give it a few seconds"; break;
        case State::FarAway: line = "far away, press again when you're together"; break;
        case State::JustDied: line = "just died, press again after they respawn"; break;
        default: line = Repair(m, c, st, lobby, unmatched, name, true); break;
        }
        if (!report.empty()) report += "\n";
        report += name + ": " + line;
    }
    if (report.empty()) report = "No other players in the session";
    if (!s_Revived.empty()) s_FollowUpAt = now + 3000;
    SetMessage(report);
}

// 3 s after a revive: still not right -> rebuild
static void FollowUp(ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    if (!InSession(lobby) || !main) return;
    std::vector<Member> members = Members();
    std::vector<Copy> set = PlayerSet();
    TrackCopies(set, now);
    uintptr_t gdm = Global(GAME_DATA_MAN);
    bool boss = gdm && Rd<uint8_t>(gdm + 0xc0);
    std::string report;
    for (const Revived& r : s_Revived) {
        for (const Member& m : members) {
            if (m.isLocal || m.steamId != r.steamId) continue;
            const Copy* c = CopyOf(m, members, set, main);
            State st = c ? Classify(*c, main, now) : State::NoCharacter;
            // dead again (they really are), far away or fine: a rebuild now would only make it worse
            if (!c || (st != State::Hidden && st != State::Faded && st != State::BuiltWrong && st != State::NotLoaded)) break;
            if (!r.mayRebuild && boss) {
                Log("CoopResync: auto: %s still stuck after the revive; no rebuild in a boss fight", r.name.c_str());
                break;
            }
            std::string why = Rebuild(m, c->chr, lobby);
            if (!report.empty()) report += "\n";
            report += r.name + ": " + (why.empty() ? std::string("still stuck, rebuilding their character") : why);
        }
    }
    s_Revived.clear();
    if (!report.empty()) SetMessage(report);
}

// A block id and a position inside that block, the form reloads and PlayerIns+0x6c0/+0x6d0 use
struct Spot {
    int32_t block;
    float x, y, z, yaw;
    bool Valid() const {
        return block != -1 && block != 0 && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(yaw);
    }
};

// Any character's spot, worked out the way 0x65b1b0 does it for the main player; false while the
// character's reference block is unset (a copy before its origin arrives)
static bool SpotOf(uintptr_t chr, Spot& spot) {
    if (!s_SpotOk) return false;
    int32_t block = -1;
    reinterpret_cast<int32_t* (*)(uintptr_t, int32_t*)>(Addr(REF_BLOCK))(chr, &block);
    if (block == -1) return false;
    alignas(16) uint8_t buf[0x20] = {};
    alignas(16) float pos[4] = {};
    const float* phys = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(chr, buf);
    if (!phys || !reinterpret_cast<bool (*)(float*, const float*, int32_t*)>(Addr(TO_BLOCK_POS))(pos, phys, &block)) return false;
    alignas(16) uint8_t rbuf[0x20] = {};
    const float* rot = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(ROTATION))(chr, rbuf);
    int32_t fine = -1;
    const int32_t* id = reinterpret_cast<const int32_t* (*)(int32_t*, const int32_t*)>(Addr(NORM_BLOCK))(&fine, &block);
    spot = { id ? *id : -1, pos[0], pos[1], pos[2], rot ? rot[1] : 0.0f };
    return spot.Valid();
}

// SpotOf for checks and diagnostics, which must never take the feature down
static bool SafeSpotOf(uintptr_t chr, Spot& spot) {
    __try {
        return SpotOf(chr, spot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool BreakInHostSpot(Spot& spot) {
    if (!s_HostSpotOk) return false;
    uintptr_t net = Global(CS_NET_MAN);
    uintptr_t bim = net ? Ptr(net + 0xa8) : 0;
    uintptr_t step = bim ? Ptr(bim + 0x98) : 0;
    if (!step) return false;
    spot = { Rd<int32_t>(step + 0xa8), Rd<float>(step + 0xac), Rd<float>(step + 0xb0), Rd<float>(step + 0xb4), Rd<float>(step + 0xb8) };
    return spot.Valid();
}

// The checks a reload needs; empty when it may go ahead
static std::string ReloadRefusal(uintptr_t main) {
    uintptr_t gm = Global(GAME_MAN), gdm = Global(GAME_DATA_MAN);
    if (!s_ReloadOk || !Intact(MAP_RELOAD, true) || !Intact(SET_RELOAD_BLOCK, true) || !Intact(SET_RELOAD_POS, true))
        return "Reload isn't available on this game version";
    if (!main || !gm) return "Wait until you're loaded in";
    if (Hp(main) <= 0) return "Not while you're dead";
    if (Rd<uint8_t>(gm + 0x10)) return "Already loading";
    if (gdm && Rd<uint8_t>(gdm + 0xc0)) return "Not during a boss fight";
    return {};
}

// The game's own warp-to-host recipe (0x5f3950) with a spot of our choosing: block, position and
// rotation for the next reload, then the reload
// A reload we started, watched until it's done, so the log says how it went (2026-10-29 a guest's game crashed
// during Go to host and the log simply ended at "Going to the host..."): started, loading, done or not
struct ReloadWatch {
    ULONGLONG at = 0;
    Spot target = {};
    std::string what;
    uintptr_t chr = 0;   // our character when it was asked for
    bool left = false;   // the load began: that character gone
};
static ReloadWatch s_ReloadWatch;
static std::string SummonsOutText();

static void ReloadAt(const Spot& spot, const char* what) {
    Log("CoopResync: %s: block %08X (%.2f, %.2f, %.2f) yaw %.2f; summons here: %s", what, (uint32_t)spot.block, spot.x, spot.y,
        spot.z, spot.yaw, SummonsOutText().c_str());
    auto setBlock = reinterpret_cast<int32_t* (*)(int32_t*, const int32_t*)>(Addr(SET_RELOAD_BLOCK));
    auto setPos = reinterpret_cast<void (*)(const float*, const float*)>(Addr(SET_RELOAD_POS));
    auto reload = reinterpret_cast<uint32_t (*)()>(Addr(MAP_RELOAD));
    int32_t out = 0;
    int32_t block = spot.block;
    setBlock(&out, &block);
    // a little above the ground, so the reload never starts us inside it
    alignas(16) float pos[4] = { spot.x, spot.y + 0.5f, spot.z, 1.0f };
    alignas(16) float rot[4] = { 0.0f, spot.yaw, 0.0f, 1.0f };
    setPos(pos, rot);
    uint32_t started = reload();
    Log("CoopResync: %s: reload asked for (result %u)", what, started);
    s_ReloadWatch = {};
    s_ReloadWatch.at = GetTickCount64();
    s_ReloadWatch.target = spot;
    s_ReloadWatch.what = what;
    s_ReloadWatch.chr = MainPlayer();
}

// Where we stand for a reload: the last safe ground (PlayerIns safe_block_id +0x6e4 and
// safe_block_pos +0x6d4; unset right after a reload, then the current block +0x6d0 and
// block_position +0x6c0), facing the same way (block_position yaw +0x6cc)
static bool OurSpot(uintptr_t main, Spot& spot) {
    int32_t block = Rd<int32_t>(main + 0x6e4);
    uintptr_t at = main + 0x6d4;
    if (block == -1 || block == 0) {
        block = Rd<int32_t>(main + 0x6d0);
        at = main + 0x6c0;
    }
    spot = { block, Rd<float>(at), Rd<float>(at + 4), Rd<float>(at + 8), Rd<float>(main + 0x6cc) };
    if (!std::isfinite(spot.yaw)) spot.yaw = 0.0f;
    return spot.Valid();
}

static void ReloadHere() {
    uintptr_t main = MainPlayer();
    std::string no = ReloadRefusal(main);
    if (!no.empty()) return SetMessage(no);
    Spot spot;
    if (!OurSpot(main, spot)) return SetMessage("Couldn't tell where you are");
    ReloadAt(spot, "reload here");
    SetMessage("Reloading here...");
}

// The boss fight flag can stay on after a fight ends (2026-10-31, twice: after a Morgott attempt, and a Bell Bearing
// Hunter with no bar up), which keeps graces shut (stuck in Melina's talk) and refuses every reload. Only by hand: a
// real fight puts it back when it starts
static void ClearBossFlag() {
    uintptr_t gdm = Global(GAME_DATA_MAN);
    if (!gdm || !Rd<uint8_t>(gdm + 0xc0)) return SetMessage("The boss fight flag is already off");
    Log("CoopResync: clearing the boss fight flag: on for %.0f s, boss %d (NpcParam %d)", Rd<float>(gdm + 0xb8), Rd<int32_t>(gdm + 0xc8),
        Rd<int32_t>(gdm + 0xcc));
    *reinterpret_cast<volatile uint8_t*>(gdm + 0xc0) = 0;
    SetMessage("Boss fight flag cleared");
}

// ---- the overlay link ----
// Our own Steam messages between the party's overlays. Seamless uses channels 40 and 80 and
// accepts every peer's session, so a channel of our own reaches the same players. Each overlay
// tells the others where it stands about once a second; a far-off player's character here has no
// position at all (local path, every block -1), so this is how a guest learns where the host is.
// Each game is the authority on its own player, so every half second it also says where that
// player is right now ("VSWL", block + position in it, as PlayerIns+0x6d0/+0x6c0 keep them); the
// automatic resync rebuilds a copy here that stays far from that (see "automatic resync").

static constexpr int kLinkChannel = 0x5753;
static constexpr uint32_t kSpotMagic = 0x53575356;  // "VSWS": their last safe ground (where a reload puts you)
static constexpr uint32_t kLiveMagic = 0x4C575356;  // "VSWL": where they are right now (same layout)
static constexpr uint16_t kSpotVersion = 1;
static constexpr ULONGLONG kSpotEveryMs = 1000, kSpotFreshMs = 5000, kLiveEveryMs = 500, kLiveFreshMs = 2000;
enum : uint16_t { kSpotDead = 1, kSpotLoading = 2, kSpotBoss = 4 };

#pragma pack(push, 1)
struct SpotMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    int32_t block;
    float x, y, z, yaw;
};
#pragma pack(pop)

// SteamNetworkingIdentity (136 bytes); type 16 = SteamID
struct SteamIdentity {
    int32_t type;
    int32_t size;
    uint64_t steamId;
    uint8_t rest[120];
};
static_assert(sizeof(SteamIdentity) == 136, "SteamNetworkingIdentity is 136 bytes");

using NetMessagesFn = void* (*)();
using SendFn = int (*)(void* self, const SteamIdentity* to, const void* data, uint32_t size, int flags, int channel);
using ReceiveFn = int (*)(void* self, int channel, void** out, int max);
using ReleaseFn = void (*)(void* message);
static NetMessagesFn s_NetMessages = nullptr;
static SendFn s_Send = nullptr;
static ReceiveFn s_Receive = nullptr;
static ReleaseFn s_Release = nullptr;

struct Heard {
    uint64_t steamId;
    Spot spot;        // VSWS
    uint16_t flags;
    ULONGLONG at;
    Spot live;        // VSWL
    ULONGLONG liveAt; // 0 = never (an older overlay)
};
static std::vector<Heard> s_Heard;
static std::vector<uint64_t> s_SendLogged;
static ULONGLONG s_NextSpotSend = 0, s_NextLiveSend = 0;

static void ResolveLink() {
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam) return;
    s_NetMessages = reinterpret_cast<NetMessagesFn>(GetProcAddress(steam, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"));
    s_Send = reinterpret_cast<SendFn>(GetProcAddress(steam, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"));
    s_Receive = reinterpret_cast<ReceiveFn>(GetProcAddress(steam, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"));
    s_Release = reinterpret_cast<ReleaseFn>(GetProcAddress(steam, "SteamAPI_SteamNetworkingMessage_t_Release"));
}

static bool LinkOk() { return s_NetMessages && s_Send && s_Receive && s_Release; }

static const Heard* HeardFrom(uint64_t steamId, ULONGLONG now) {
    for (const Heard& h : s_Heard)
        if (h.steamId == steamId && now - h.at < kSpotFreshMs) return &h;
    return nullptr;
}

static void SendSpot(ULONGLONG now) {
    bool spotDue = now >= s_NextSpotSend, liveDue = now >= s_NextLiveSend;
    if (!spotDue && !liveDue) return;
    if (spotDue) s_NextSpotSend = now + kSpotEveryMs;
    if (liveDue) s_NextLiveSend = now + kLiveEveryMs;
    void* net = s_NetMessages();
    if (!net) return;
    std::vector<Member> members = Members();
    uintptr_t main = MainPlayer(), gm = Global(GAME_MAN), gdm = Global(GAME_DATA_MAN);
    SpotMsg msg = { kSpotMagic, kSpotVersion, 0, -1, 0.0f, 0.0f, 0.0f, 0.0f };
    Spot spot;
    if (main && OurSpot(main, spot)) {
        msg.block = spot.block;
        msg.x = spot.x, msg.y = spot.y, msg.z = spot.z, msg.yaw = spot.yaw;
    }
    if (!main || (gm && Rd<uint8_t>(gm + 0x10))) msg.flags |= kSpotLoading;
    if (main && Hp(main) <= 0) msg.flags |= kSpotDead;
    if (gdm && Rd<uint8_t>(gdm + 0xc0)) msg.flags |= kSpotBoss;
    SpotMsg live = { kLiveMagic, kSpotVersion, msg.flags, -1, 0.0f, 0.0f, 0.0f, 0.0f };
    if (main) {
        live.block = Rd<int32_t>(main + 0x6d0);
        live.x = Rd<float>(main + 0x6c0), live.y = Rd<float>(main + 0x6c4), live.z = Rd<float>(main + 0x6c8);
        live.yaw = Rd<float>(main + 0x6cc);
    }
    for (const Member& m : members) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        if (liveDue && live.block != -1) s_Send(net, &to, &live, sizeof(live), 32, kLinkChannel);
        if (!spotDue) continue;
        int result = s_Send(net, &to, &msg, sizeof(msg), 32 /* unreliable, restart broken sessions */, kLinkChannel);
        bool logged = false;
        for (uint64_t id : s_SendLogged) logged |= id == m.steamId;
        if (!logged) {
            s_SendLogged.push_back(m.steamId);
            Log("CoopResync: link: first spot sent to %llX (result %d)", (unsigned long long)m.steamId, result);
        }
    }
}

static void OnLinkMessage(uint64_t from, const uint8_t* data, int size, ULONGLONG now);  // invasions, Horse Up

// Horse Up ("VSWH"): we got on Torrent with this song (horse_up.h); sent once, reliably. 48 bytes, a size no other
// message has, so older overlays drop it
static constexpr uint32_t kHorseMagic = 0x48575356;
#pragma pack(push, 1)
struct HorseMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char song[40];
};
#pragma pack(pop)
static_assert(sizeof(HorseMsg) == 48, "HorseMsg is 48 bytes");

// SteamNetworkingMessage_t: +0 data, +8 size, +0x10 sender identity (type, size, Steam ID at +0x18)
static void ReceiveSpots(ULONGLONG now) {
    void* net = s_NetMessages();
    if (!net) return;
    void* msgs[16];
    for (int round = 0; round < 8; round++) {
        int n = s_Receive(net, kLinkChannel, msgs, 16);
        for (int i = 0; i < n; i++) {
            uintptr_t m = reinterpret_cast<uintptr_t>(msgs[i]);
            const uint8_t* data = Rd<const uint8_t*>(m);
            int size = Rd<int32_t>(m + 8);
            uint64_t from = Rd<int32_t>(m + 0x10) == 16 ? Rd<uint64_t>(m + 0x18) : 0;
            SpotMsg msg;
            // other messages can be the same size (the invaders' armor), so the magic decides
            uint32_t magic = from && data && size == (int)sizeof(msg) ? Rd<uint32_t>(reinterpret_cast<uintptr_t>(data)) : 0;
            if (magic == kSpotMagic || magic == kLiveMagic) {
                memcpy(&msg, data, sizeof(msg));
                if (msg.version == kSpotVersion) {
                    Heard* h = nullptr;
                    for (Heard& old : s_Heard)
                        if (old.steamId == from) h = &old;
                    if (!h) {
                        s_Heard.push_back({ from, { -1, 0, 0, 0, 0 }, 0, 0, { -1, 0, 0, 0, 0 }, 0 });
                        h = &s_Heard.back();
                        Log("CoopResync: link: hearing from %llX (at %08X)", (unsigned long long)from, (uint32_t)msg.block);
                    }
                    Spot s = { msg.block, msg.x, msg.y, msg.z, msg.yaw };
                    if (magic == kSpotMagic) h->spot = s, h->flags = msg.flags, h->at = now;
                    else h->live = s, h->liveAt = now;
                }
            } else if (from && data && size > 8) {
                OnLinkMessage(from, data, size, now);
            }
            s_Release(msgs[i]);
        }
        if (n < 16) break;
    }
}

// Reload next to another player: guests at the host, the host at a guest (or a guest at another). In order: their
// own report over the link, their character's live position here (when it has its block origin) and, for the host,
// the spot the game recorded when we joined. All are logged, with a check of SpotOf against the game's own numbers
// for our character. Not with an invader close by (as the game's fast travel), nor into a boss fight.
static uint64_t s_GoingTo = 0;    // whose side our last Go to reloads at (an invader of ours left behind: "on the way")
static ULONGLONG s_GoingAt = 0;
static bool InvaderOnYou(uintptr_t main);  // invasions

static void GoTo(uint64_t steamId, ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    if (!InSession(lobby)) return SetMessage("Not in a co-op session");
    std::string no = ReloadRefusal(main);
    if (!no.empty()) return SetMessage(no);
    if (InvaderOnYou(main)) return SetMessage("Not with an invader on you");

    std::vector<Member> members = Members();
    const Member* to = nullptr;
    for (const Member& m : members)
        if (!m.isLocal && m.steamId == steamId) to = &m;
    if (!to) return SetMessage("They're no longer in the session");
    std::vector<Copy> set = PlayerSet();
    const Copy* c = CopyOf(*to, members, set, main);
    std::string who = to->isHost ? std::string("The host") : NameOf(*to, c);
    const Heard* told = HeardFrom(to->steamId, now);
    if (told && (told->flags & kSpotDead)) return SetMessage(who + " is down, try again once they've respawned");
    if (told && (told->flags & kSpotLoading)) return SetMessage(who + " is loading, try again in a moment");
    if (told && (told->flags & kSpotBoss)) return SetMessage(who + " is in a boss fight, try again after it");
    if (!told && c && IsDead(c->chr)) return SetMessage(who + " is down, try again once they've respawned");

    Spot live = {}, joined = {}, self = {};
    bool haveTold = told && told->spot.Valid();
    bool haveLive = c && SpotOf(c->chr, live);
    bool haveJoined = to->isHost && BreakInHostSpot(joined);
    if (SpotOf(main, self))
        Log("CoopResync: spot check on us: worked out %08X (%.2f, %.2f, %.2f) yaw %.2f, game has %08X (%.2f, %.2f, %.2f) yaw %.2f",
            (uint32_t)self.block, self.x, self.y, self.z, self.yaw, Rd<uint32_t>(main + 0x6d0), Rd<float>(main + 0x6c0),
            Rd<float>(main + 0x6c4), Rd<float>(main + 0x6c8), Rd<float>(main + 0x6cc));
    if (haveTold)
        Log("CoopResync: %llX's own report: %08X (%.2f, %.2f, %.2f)", (unsigned long long)to->steamId, (uint32_t)told->spot.block,
            told->spot.x, told->spot.y, told->spot.z);
    if (haveLive)
        Log("CoopResync: %llX from their character: %08X (%.2f, %.2f, %.2f)", (unsigned long long)to->steamId, (uint32_t)live.block,
            live.x, live.y, live.z);
    if (haveJoined)
        Log("CoopResync: host from the join record: %08X (%.2f, %.2f, %.2f)", (uint32_t)joined.block, joined.x, joined.y, joined.z);

    const char* what = to->isHost ? "go to host" : "go to guest";
    std::string how;
    Spot spot;
    if (haveTold) spot = told->spot, how = " (their report)";
    else if (haveLive) spot = live, how = " (their character)";
    else if (haveJoined) spot = joined, how = " (join record)";
    else return SetMessage("Can't tell where " + (to->isHost ? std::string("the host is") : who + " is") + ": are they on the latest pack?");
    ReloadAt(spot, (what + how).c_str());
    s_GoingTo = to->steamId;
    s_GoingAt = now;
    SetMessage(to->isHost ? std::string("Going to the host...") : "Going to " + who + "...");
}

// Maps set into an open world: legacy dungeons (Stormveil, Leyndell, Raya Lucaria, ...) and the
// underground are their own map files with their own origin, placed into m60/m61 at a fixed shift.
// WorldMapLegacyConvParam (table 89; how the world map shows you inside Stormveil) pairs a point in
// such a map with the same point on an open-world tile. A real legacy dungeon's rows all give one
// shift (Stormveil's six agree to the centimetre). Catacombs, caves and tunnels are separate spaces
// that are only pinned to their entrance's tile for the map (whole tiles, no height), and Farum
// Azula's rows disagree, so those stay apart; Roundtable Hold (m11_10) has a single row but is a
// separate space too.
struct MapShift {
    uint32_t map;    // block id without its last byte
    uint32_t world;  // 60 (the Lands Between) or 61 (the Realm of Shadow)
    float d[3];
};
static std::vector<MapShift> s_Shifts;
static bool s_ShiftsRead = false;

static void ReadMapShifts() {
    uintptr_t file = ParamFile(89, L"WorldMapLegacyConvParam");
    if (!file) return;  // not loaded yet: again later
    s_ShiftsRead = true;
    struct Found {
        uint32_t map, world;
        float d[3];
        bool bad;
    };
    std::vector<Found> found;
    int rows = Rd<uint16_t>(file + 0xa);
    for (int i = 0; i < rows && i < 4000; i++) {
        uintptr_t r = file + Rd<uint64_t>(file + 0x48 + i * 0x18);
        uint32_t sa = Rd<uint8_t>(r + 4), sx = Rd<uint8_t>(r + 5), sz = Rd<uint8_t>(r + 6);
        uint32_t da = Rd<uint8_t>(r + 0x14), dx = Rd<uint8_t>(r + 0x15), dz = Rd<uint8_t>(r + 0x16);
        if ((da != 60 && da != 61) || (sa == 11 && sx == 10)) continue;
        float d[3] = { dx * 256.0f + Rd<float>(r + 0x18) - Rd<float>(r + 8), Rd<float>(r + 0x1c) - Rd<float>(r + 0xc),
                       dz * 256.0f + Rd<float>(r + 0x20) - Rd<float>(r + 0x10) };
        if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2])) continue;
        // pinned to (within half a metre of) a whole tile with no height: a separate space shown on the map
        if (fabsf(d[1]) < 0.5f && fabsf(remainderf(d[0], 256.0f)) < 0.5f && fabsf(remainderf(d[2], 256.0f)) < 0.5f) continue;
        uint32_t map = sa << 24 | sx << 16 | sz << 8;
        Found* f = nullptr;
        for (Found& o : found)
            if (o.map == map && o.world == da) f = &o;
        if (!f) found.push_back({ map, da, { d[0], d[1], d[2] }, false });
        else if (fabsf(f->d[0] - d[0]) > 2.0f || fabsf(f->d[1] - d[1]) > 2.0f || fabsf(f->d[2] - d[2]) > 2.0f) f->bad = true;
    }
    std::string list, apart;
    for (const Found& f : found) {
        char b[24];
        snprintf(b, sizeof(b), "m%02u_%02u_%02u", f.map >> 24, (f.map >> 16) & 0xff, (f.map >> 8) & 0xff);
        std::string& to = f.bad ? apart : list;
        to += (to.empty() ? "" : " ") + std::string(b);
        if (!f.bad) s_Shifts.push_back({ f.map, f.world, { f.d[0], f.d[1], f.d[2] } });
    }
    Log("CoopResync: map shifts: %zu maps set into the open world (%s); rows disagree, left apart: %s", s_Shifts.size(),
        list.c_str(), apart.empty() ? "none" : apart.c_str());
}

// A spot in its open world's frame: tiles (m60/m61 block ids 0xAAXXZZ00, 256 m) by their index, maps
// set into the world by their shift; false for a separate space (or an unknown block)
static bool WorldPos(const Spot& s, uint32_t& world, float out[3]) {
    uint32_t b = static_cast<uint32_t>(s.block), area = b >> 24;
    if (s.block == -1 || s.block == 0) return false;
    if (area == 60 || area == 61) {
        if (b & 0xff) return false;
        world = area;
        out[0] = s.x + 256.0f * ((b >> 16) & 0xff), out[1] = s.y, out[2] = s.z + 256.0f * ((b >> 8) & 0xff);
        return true;
    }
    for (const MapShift& m : s_Shifts) {
        if (m.map != (b & 0xffffff00)) continue;
        world = m.world;
        out[0] = s.x + m.d[0], out[1] = s.y + m.d[1], out[2] = s.z + m.d[2];
        return true;
    }
    return false;
}

// Metres between two spots: in the same block, or both in the same open world (tiles and the maps set
// into it); -1 when they can't be measured against each other (separate spaces)
static float SpotMetres(const Spot& a, const Spot& b) {
    float pa[3] = { a.x, a.y, a.z }, pb[3] = { b.x, b.y, b.z };
    if (a.block != b.block) {
        uint32_t wa = 0, wb = 0;
        if (!WorldPos(a, wa, pa) || !WorldPos(b, wb, pb) || wa != wb) return -1.0f;
    }
    float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    return std::isfinite(d) ? d : -1.0f;
}

// Every frame while a reload of ours is under way (ReloadAt)
static void WatchReload(ULONGLONG now) {
    ReloadWatch& w = s_ReloadWatch;
    if (!w.at) return;
    // GameMan+0x10 is only set until the fade-out begins; the load itself is our character going (rig 2026-10-30: gone
    // 0.8 s after asking, a new one 2.5 s, placed in a block 3.9 s)
    uintptr_t main = MainPlayer();
    ULONGLONG ms = now > w.at ? now - w.at : 0;  // the frame's clock was read before the reload was asked for
    if (!main || main != w.chr) {
        if (!w.left) Log("CoopResync: %s: loading (%llu ms after the reload was asked for)", w.what.c_str(), (unsigned long long)ms);
        w.left = true;
        w.chr = 0;
    }
    if (!main || (w.left && Rd<int32_t>(main + 0x6d0) == -1)) {
        if (ms > 90000) {
            Log("CoopResync: %s: still loading %llu s on; no longer watched", w.what.c_str(), (unsigned long long)(ms / 1000));
            w = {};
        }
        return;
    }
    if (!w.left && ms < 15000) return;
    Spot here = { -1, 0, 0, 0, 0 };
    bool have = SpotOf(main, here);
    float d = have ? SpotMetres(here, w.target) : -1.0f;
    Log("CoopResync: %s: %s %.1f s after it was asked for: at %08X (%.2f, %.2f, %.2f), %s from where it was aimed", w.what.c_str(),
        w.left ? "done" : "NEVER STARTED LOADING", ms / 1000.0f, (uint32_t)here.block, here.x, here.y, here.z,
        d >= 0.0f ? (std::to_string((int)d) + " m").c_str() : "? m");
    w = {};
}

// ---- automatic resync ----
// "Resync players" by itself (the menu's switch, on by default), touching a player only when it can't
// make things worse. Two triggers:
// - every 10 s, a copy here that has needed a repair (by its own signs, as for the button) for 5 s;
// - any time, a copy here that has stayed more than 40 m from where their own game says they are
//   (VSWL, each game is the authority on its own player) for 6 s: rebuilt. Network lag is well under
//   that (half-second reports; a galloping horse covers about 10 m in that time).
// Either way only when their overlay says over the link (within 5 s) that they're alive, not loading
// and within 150 m of us, we're loaded in and alive, and in a boss fight only a revive, no rebuild.
// After a repair that player is left alone for 30 s, and after three in five minutes for the rest of
// those five minutes, so a repair that doesn't take isn't repeated forever.

static constexpr ULONGLONG kAutoEveryMs = 10000, kAutoBadForMs = 5000, kAutoCalmMs = 30000, kAutoWindowMs = 300000;
static constexpr float kOffMetres = 40.0f;
static constexpr ULONGLONG kOffForMs = 6000;
// The F8 switches: all of it, and the position trigger on its own; kept in Coop.ini in the per-user
// settings folder, so a switch turned off stays off across launches and pack updates
static std::atomic<bool> s_AutoResync{ true }, s_PositionCheck{ true };

static std::atomic<bool> s_SettingsLoaded{ false };

static void LoadCoopSettings() {
    if (s_SettingsLoaded.exchange(true)) return;  // the native menu may ask first
    char path[MAX_PATH], line[128];
    if (!Config::UserFile("Coop.ini", path, sizeof(path))) return;
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            char key[64];
            int v;
            if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2) continue;
            if (!strcmp(key, "auto_resync")) s_AutoResync = v != 0;
            else if (!strcmp(key, "position_check")) s_PositionCheck = v != 0;
        }
        fclose(f);
    }
    Log("CoopResync: auto-resync %s, position check %s (%s)", s_AutoResync ? "on" : "off", s_PositionCheck ? "on" : "off", path);
}

static void SaveCoopSettings() {
    char path[MAX_PATH];
    if (!Config::UserFile("Coop.ini", path, sizeof(path))) return;
    FILE* f = fopen(path, "w");
    if (!f) return Log("CoopResync: couldn't save %s", path);
    fprintf(f, "# Co-op switches from the F8 menu (1 = on, 0 = off)\n");
    fprintf(f, "auto_resync = %d\n", s_AutoResync ? 1 : 0);
    fprintf(f, "position_check = %d\n", s_PositionCheck ? 1 : 0);
    fclose(f);
    Log("CoopResync: auto-resync %s, position check %s (saved)", s_AutoResync ? "on" : "off", s_PositionCheck ? "on" : "off");
}

struct AutoTrack {
    uint64_t steamId;
    State state;
    ULONGLONG since;                // in this state since
    std::vector<ULONGLONG> repairs; // our repairs of them in the last five minutes
    std::string held;               // why the current state is left alone (logged once)
    ULONGLONG offSince = 0;         // our copy has been far from their own word since (0 = it isn't)
    float off = 0;
    std::string offHeld;
};
static std::vector<AutoTrack> s_AutoTrack;
static ULONGLONG s_NextAutoLook = 0, s_NextAutoRun = 0;

static bool NeedsRepair(State s) {
    return s == State::NoCharacter || s == State::Faded || s == State::Hidden || s == State::NotLoaded || s == State::BuiltWrong;
}

static std::string MetresText(float d) { return d >= 0.0f ? std::to_string((int)d) + " m" : std::string("? m"); }

// Every second: each player's state here and our copy against their word; repairs when due and safe
static void AutoResync(ULONGLONG now) {
    if (now < s_NextAutoLook) return;
    s_NextAutoLook = now + 1000;
    int lobby = LobbyState();
    uintptr_t main = MainPlayer(), gm = Global(GAME_MAN), gdm = Global(GAME_DATA_MAN);
    if (!s_ShiftsRead && main) ReadMapShifts();
    if (!s_AutoResync || !InSession(lobby) || !main) {
        s_AutoTrack.clear();
        return;
    }
    bool run = now >= s_NextAutoRun;
    if (run) s_NextAutoRun = now + kAutoEveryMs;
    bool usable = Hp(main) > 0 && !(gm && Rd<uint8_t>(gm + 0x10)) && !s_FollowUpAt;
    bool boss = gdm && Rd<uint8_t>(gdm + 0xc0);
    Spot me = { Rd<int32_t>(main + 0x6d0), Rd<float>(main + 0x6c0), Rd<float>(main + 0x6c4), Rd<float>(main + 0x6c8), 0.0f };

    std::vector<Member> members = Members();
    std::vector<Copy> set = PlayerSet();
    TrackCopies(set, now);
    int unmatched = 0;
    for (const Copy& c : set)
        if (c.chr != main && !OwnerOf(c.chr, members)) unmatched++;

    std::vector<AutoTrack> kept;
    std::string report;
    for (const Member& m : members) {
        if (m.isLocal || !m.steamId) continue;
        const Copy* c = CopyOf(m, members, set, main);
        State st = c ? Classify(*c, main, now) : State::NoCharacter;
        AutoTrack t = { m.steamId, st, now, {}, {} };
        for (const AutoTrack& old : s_AutoTrack) {
            if (old.steamId != m.steamId) continue;
            t.repairs = old.repairs;
            t.offSince = old.offSince, t.offHeld = old.offHeld;
            if (old.state == st) t.since = old.since, t.held = old.held;
        }
        while (!t.repairs.empty() && now - t.repairs.front() > kAutoWindowMs) t.repairs.erase(t.repairs.begin());
        const Heard* h = HeardFrom(m.steamId, now);
        bool live = h && h->liveAt && now - h->liveAt < kLiveFreshMs && !(h->flags & (kSpotDead | kSpotLoading));

        // our copy against their own word (only for a copy that looks fine by its own signs, has a place
        // we can tell, and is in a block we can measure against theirs)
        Spot cs;
        float off = -1.0f;
        if (s_PositionCheck && c && live && (st == State::Visible || st == State::FarAway) && SafeSpotOf(c->chr, cs))
            off = SpotMetres(cs, h->live);
        if (off > kOffMetres) {
            if (!t.offSince) t.offSince = now;
            t.off = off;
        } else {
            t.offSince = 0;
            t.offHeld.clear();
        }

        // why a repair must wait ("" = it may go ahead)
        auto hold = [&](float d, bool rebuild) -> std::string {
            if (!h) return "no word from their overlay";
            if (h->flags & kSpotDead) return "they're dead";
            if (h->flags & kSpotLoading) return "they're loading";
            if (d < 0.0f) return "they're in another area";
            if (d > kNearMetres) return "they're " + MetresText(d) + " away";
            if (t.repairs.size() >= 3) return "repaired three times in five minutes";
            if (!t.repairs.empty() && now - t.repairs.back() < kAutoCalmMs) return "repaired moments ago";
            if (boss && rebuild) return "no rebuild in a boss fight";
            return {};
        };
        std::string line, name;
        if (run && usable && NeedsRepair(st) && now - t.since >= kAutoBadForMs) {
            name = NameOf(m, c);
            float d = h ? SpotMetres(me, live ? h->live : h->spot) : -1.0f;
            std::string why = hold(d, st != State::Faded);
            if (!why.empty()) {
                if (why != t.held) Log("CoopResync: auto: %s's character here is %s; left alone (%s)", name.c_str(), StateLabel(st), why.c_str());
                t.held = why;
            } else {
                line = Repair(m, c, st, lobby, unmatched, name, !boss);
                Log("CoopResync: auto: %s's character here was %s for %llu s (they're %s away): %s", name.c_str(), StateLabel(st),
                    (unsigned long long)((now - t.since) / 1000), MetresText(d).c_str(), line.c_str());
            }
        } else if (usable && t.offSince && now - t.offSince >= kOffForMs) {
            name = NameOf(m, c);
            float d = SpotMetres(me, h->live);
            std::string why = hold(d, true);
            if (!why.empty()) {
                if (why != t.offHeld)
                    Log("CoopResync: auto: %s's character here is %s from where their game says they are; left alone (%s)", name.c_str(),
                        MetresText(t.off).c_str(), why.c_str());
                t.offHeld = why;
            } else {
                std::string failed = Rebuild(m, c->chr, lobby);
                line = failed.empty() ? "out of place, rebuilding their character" : failed;
                Log("CoopResync: auto: %s's character here (%s) was %s from where their game says they are for %llu s: ours at %08X "
                    "(%.1f, %.1f, %.1f), theirs at %08X (%.1f, %.1f, %.1f), they're %s from me: %s", name.c_str(), StateLabel(st),
                    MetresText(t.off).c_str(), (unsigned long long)((now - t.offSince) / 1000), (uint32_t)cs.block, cs.x, cs.y, cs.z,
                    (uint32_t)h->live.block, h->live.x, h->live.y, h->live.z, MetresText(d).c_str(), line.c_str());
            }
        }
        if (!line.empty()) {
            if (!report.empty()) report += "\n";
            report += name + ": " + line;
            t.repairs.push_back(now);
            t.since = now;
            t.held.clear();
            t.offSince = 0;
            t.offHeld.clear();
        }
        kept.push_back(t);
    }
    s_AutoTrack = std::move(kept);
    if (!s_Revived.empty() && !s_FollowUpAt) s_FollowUpAt = now + 3000;
    if (!report.empty()) SetMessage("Auto-resync:\n" + report);
}

// ---- invasions ----
// AI Tarnished the whole party can see and fight (er-invasions/library.toml -> game data rows; the
// Invaders.txt next to the DLL lists them). They are spirit-ash buddies, which the game and Seamless
// Co-op already show on every player's game. The host summons one from code under a made-up owner
// number (players are 1-6), so nobody's own summon slot is used, and every game makes buddies from
// the invader NpcParam range hostile (team doesn't sync). The summon manager ([WorldChrMan+0x1e538])
// files groups by owner number (PlayerGameData+8) and every frame dismisses a group whose owner isn't
// in the player ChrSet (0x4b94b0: lookup, dismissal on a miss). That lookup now asks us: the ghost
// owner is present while one of its invaders is alive (and a few seconds after, for the death fade),
// so the game clears them away itself. Seamless Co-op already replaces that call (its hook #28), so
// ours goes in front of whatever the call reached. The invaders live on the summoner's game and vanish
// when it reloads (death respawn, fast travel), as a real invasion ends when its host dies.

// (SummonBuddyManager): summon the group of the requested SpEffect (+0x20) at +0xa0/+0xb0 for the main player
SITE(SUMMON_REQUEST, "summon the requested group", 0x4bbeb0, 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0x58);
// call <lookup>; test rax, rax; jne  (the dismissal check)
static constexpr uintptr_t DISMISS_CALL = 0x4b94fa;
static constexpr int32_t kGhostOwner = 9;
static constexpr int32_t kInvaderNpcFirst = 591000000, kInvaderNpcLast = 591009999;
static constexpr uint8_t kInvaderTeam = 27;
static constexpr ULONGLONG kGhostLingerMs = 6000;
static constexpr uint32_t kInvaderMagic = 0x49575356;  // "VSWI"
static constexpr uint32_t kArmorMagic = 0x41575356;    // "VSWA"
static constexpr ULONGLONG kArmorLeadMs = 600;         // the summon waits this long after the armor goes out
static constexpr int kCharaInitTable = 0x17;
static constexpr int kArmorFields[4] = { 0x20, 0x24, 0x28, 0x2c };  // CharaInitParam equip_Helm/Armer/Gaunt/Leg
// (model param modifier module, {0, PhantomParam id, 0, 0, 0, 0}, 0): paint a character in a phantom look.
// The game does it in each character's model update (0x3f8210, from its phantom id) but never for a
// copy of another game's buddy (its modifier list stays empty, so no look shows), so those copies get
// it from us every frame; the summoner's copy takes the look from its phantom override (+0x540)
SITE(PHANTOM_APPLY, "apply a phantom look", 0x43c010, 0x40, 0x55, 0x56, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb0, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00);
// MsgRepository lookup (repo, version, category, id) -> text or null. Our NpcParam rows' nameId is
// their own id, which has no text in any category; where the game finds none we answer with the
// invader's name (the HP bar asks category 428). Seamless Co-op hooks this too (its own texts), so
// ours goes in after it
static constexpr uintptr_t MSG_LOOKUP = 0x2671370;
static const uint8_t kMsgLookupBytes[] = { 0x3b, 0x51, 0x10, 0x73, 0x29, 0x44, 0x3b, 0x41, 0x14, 0x73, 0x23 };
// (chr, SpEffect id, don't sync): every game applies the scaling to its own copy
SITE(APPLY_SPEFFECT, "apply SpEffect", 0x3e8dc0, 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xec, 0xc0);
SITE(REMOVE_SPEFFECT, "remove SpEffect", 0x3ee2e0, 0x48, 0x8b, 0x89, 0x78, 0x01, 0x00, 0x00, 0xe9, 0x74, 0x94, 0x10, 0x00);

// Invasions.ini next to the DLL; every key optional
struct InvasionSettings {
    bool autoOn = true;           // automatic invasions (the host's game decides)
    int checkSeconds = 60;        // how often the host's game rolls
    float guaranteeMinutes = 120; // an invasion is certain this long after the last one
    float rampPower = 4;          // chance per roll = (minutes since the last / guaranteeMinutes) ^ rampPower
    int lootChance = 50;          // percent, each player, per invader killed: one random item
    float arriveMin = 40, arriveMax = 80;  // metres from the host, on ground the host walked, where they arrive
    int look = 203;               // PhantomParam row invaders are drawn in on every game (0 = none)
    bool randomArmor = true;      // a random armor set (any named piece) every spawn
    int levelMargin = 10;         // invaders up to this many levels above the host can come
    int summons[3] = { 25, 12, 6 };  // percent: the invader brings 1, 2, 3 summons
    int summonTarnished = 50;     // percent of summons that are another Tarnished (the rest spirit ashes)
    bool summonAshes = false;     // spirit ashes can come at all (2026-10-29: they never finished loading, all Tarnished)
};
static InvasionSettings s_Inv;

// HP like the game's own NPC invaders (Invaders.txt, from library.toml [area]): the human-NPC HP multiplier of
// the area the host is in, which the enemies around the host give away (each carries its area's ordinary
// scaling SpEffect), on the NPC-invader body an invader is measured against ("hpref"), reached with one of the
// "hpladder" SpEffects; plus that area's defense row
struct AreaEntry {
    int32_t plain;   // the ordinary area scaling SpEffect enemies there carry
    float hp;        // human-NPC max HP multiplier there
    int32_t defRow;  // our SpEffect with that area's human-NPC defense and resistance
};
static std::vector<AreaEntry> s_Areas;
static std::vector<std::pair<int, int32_t>> s_AreaByLevel;   // host level from -> ordinary SpEffect (no enemy seen)
static std::vector<std::pair<int32_t, float>> s_HpRef;       // NpcParam -> HP it's measured against
static int32_t s_LadderBase = 0, s_LadderCount = 0;
static float s_LadderFirst = 0.5f, s_LadderRatio = 1.025f;
static int32_t s_AreaSeen = 0;       // ordinary area SpEffect most enemies near the host carried at the last look
static ULONGLONG s_NextAreaScan = 0;

// Spirit ashes an invader can bring (Invaders.txt "ash"): hostile copies of the game's own, upgraded for the
// host's level
struct AshEntry {
    int32_t trigger;                 // summon SpEffect, + upgrade 0-10
    int levelMin;
    std::vector<int32_t> npcs;       // NpcParam rows of its characters
    std::string name;
};
static std::vector<AshEntry> s_Ashes;

struct LootItem {
    uint32_t id;   // with the category bits
    int count;
    std::string name;
};
static std::vector<LootItem> s_Loot;

// Where the host has been, for arrivals: the main player's map position (block +0x6d0, position in it +0x6c0)
// every half second. Not physics positions: the physics frame is re-centred as you travel (physics = map +
// a whole-metre offset that changes, and differs per game), so old physics positions point at thin air.
struct TrailPoint {
    uint32_t block;
    float p[4];
    ULONGLONG at;
};
static std::vector<TrailPoint> s_Trail;
static TrailPoint s_LastSample = {};
static ULONGLONG s_NextTrail = 0, s_LastInvasion = 0, s_NextCheck = 0;
static float s_ChanceNow = 0;

struct InvaderEntry {
    int32_t buddy, npc, chara, levelMin, levelMax, level;
    std::string style, title;
};
// Invaders.txt "scaling <base> <levels per step> <last step>": toughness SpEffect = base + 1000 * party + step
static int32_t s_ScaleBase = 0, s_ScaleStep = 5, s_ScaleLast = 0;

struct ArmorPiece {
    int32_t id;
    std::string name;
};
static std::vector<ArmorPiece> s_Armor[4];  // InvaderArmor.txt by slot: head, body, arms, legs

// The host's invasion between sending the armor and summoning: the invader, then its summons
struct PendingMember {
    const InvaderEntry* pick = nullptr;   // a Tarnished
    const AshEntry* ash = nullptr;        // or a spirit ash
    std::string name;
    int bucket = 0;
    int32_t armor[4] = {};                // what its row is dressed in (dressed: it is)
    bool dressed = false;
    // brought back (see "invaders that carry on alone"): where it stands on the other game and what it had
    Spot spot = { -1, 0, 0, 0, 0 };
    int hp = 0;
    std::string title;
    int32_t hpRow = 0, defRow = 0;
    bool summon = false;
    int company = 0;
};
struct PendingInvasion {
    bool on = false;
    std::vector<PendingMember> members;
    bool automatic = false;
    ULONGLONG at = 0;
    int tries = 0;   // automatic: times it found no ground to arrive on; back: times it waited for the host
    bool back = false;  // invaders of ours coming back from another game
};
static PendingInvasion s_Pending;
static std::vector<InvaderEntry> s_InvaderList;
static std::vector<std::string> s_InvaderNames;
static bool s_InvasionsOk = false;
static uintptr_t s_PrevDismissLookup = 0;
static bool s_NameHookTried = false;

// The names the game's lookup gives our NpcParam rows: the build's title until an invader of that
// row is named (any thread reads, the task thread writes; a torn read is a garbled name for a frame)
struct NameSlot {
    std::atomic<int32_t> id{ 0 };
    wchar_t text[48];
};
static NameSlot s_NameSlots[192];
static int s_NextNameSlot = 0;
using MsgLookupFn = const wchar_t* (*)(void*, uint32_t, uint32_t, int32_t);
static MsgLookupFn s_OrigMsgLookup = nullptr;
static std::atomic<uint64_t> s_NameCategories[16];  // text categories we have answered (bits), for the log
static std::atomic<uint32_t> s_NewNameCategory{ 0 };  // + 1
static std::atomic<ULONGLONG> s_LastInvaderAlive{ 0 };
static std::atomic<bool> s_WantInvader{ false };

// What we know about the invaders in play (task thread)
struct ActiveInvader {
    uint64_t handle;
    std::string name, title;
    ULONGLONG seen;    // last time it was in the buddy set
    int sends;         // announcements left to send (host)
    ULONGLONG nextSend;
    bool own = false;          // summoned by this game
    bool rewarded = false;     // this player's loot roll is done
    int bucket = -1;           // level catch-up steps (the summoner's game says; -1 = not yet)
    int32_t hpRow = 0, defRow = 0;  // its HP and area defense SpEffects (the summoner's game decides; 0 = none)
    bool hpDecided = false;    // own invader: hpRow/defRow are set
    bool ash = false;          // a spirit ash an invader brought (no announcement, no loot)
    bool summon = false;       // came with another invader
    int company = 0;           // the invader: how many summons came with it
    int32_t applied[3] = {};   // SpEffects on its copy here: toughness, HP, area defense
    ULONGLONG since = 0;       // first seen here
    ULONGLONG topUpAt = 0;     // own invader: fill HP to the scaled maximum then
    bool named = false;        // name and title are the summoner's (not a placeholder)
    bool nameShown = false;    // the game's name lookup gives it
    bool looked = false;       // logged its look
    // diagnostics (see "invasion diagnostics")
    uintptr_t chr = 0;         // its copy here this frame (0 = none)
    ULONGLONG appeared = 0;    // when its copy last appeared here
    ULONGLONG newsAt = 0;      // when the summoner's announcement arrived
    ULONGLONG nextStatus = 0, nextReport = 0;
    int statusLines = 0, reports = 0, lastHp = 0;
    bool painted = false;      // another game's copy took the look this frame
    bool missingLogged = false;
    // invaders that carry on alone
    const InvaderEntry* build = nullptr;  // own: what it was summoned as, to bring it back
    const AshEntry* ashEntry = nullptr;
    int32_t armor[4] = {};
    bool dressed = false;
    int maxHp = 0;
    int resumeHp = 0;          // own, brought back: the HP it had (the top-up fills to that)
    bool back = false;         // own: brought back (its announcement brings no banner)
    bool orphan = false;       // another game's: its summoner's game no longer has it, it carries on here alone
    int goneLines = 0;         // lines logged about that
};
static std::vector<ActiveInvader> s_Active;

// ---- invaders that carry on alone ----
// When the summoner dies (or reloads) its game drops its invaders, but another game's copies stay (the dismissal
// check keeps live invaders) and fight that player on their own: the summoner's game has no copy and can't see
// them (2026-10-30: the host died, the guest fought three more for a minute). That's kept as a feature: the other
// player fights on or runs (150 m from all of them for 5 s and they're dropped there). The summoner's game keeps
// what each was (s_GoneOwn), hears from that game's reports where it is and its HP, shows them in the menu, and
// starts no new invasion meanwhile. Once the summoner is back within kReturnMetres of them, alive and free, the
// other game drops its copies ("VSWG" drop; a summoner's group goes whole there) and the summoner's game summons
// them again where they stand, with the HP they have left. A copy's last report says it's gone (no kRepInSet).
struct GoneInvader {
    uint64_t handle;
    ULONGLONG at;              // its copy left here
    bool died;                 // we were dead then
    ActiveInvader was;
    // the other game's copy, as its reports say
    uint64_t holder = 0;
    std::string holderName;
    ULONGLONG heardAt = 0;     // its last report of a live copy (0 = none yet)
    Spot spot = { -1, 0, 0, 0, 0 };
    int hp = 0;
    bool asked = false;        // asked them to drop it (to bring it back here)
    bool dropped = false;      // they said they have no copy
    uint64_t comingTo = 0;     // it went with our Go to that player (we're on the way to them)
};
static std::vector<GoneInvader> s_GoneOwn;
static constexpr ULONGLONG kOrphanAfterMs = 3000;  // gone this long before a live copy elsewhere counts as one alone
static constexpr ULONGLONG kOrphanQuietMs = 8000;  // no live report this long: that copy is gone too
static constexpr float kReturnMetres = 80.0f, kFleeMetres = 150.0f;

#pragma pack(push, 1)
struct InvaderMsg {
    uint32_t magic;
    uint16_t version;  // 2: bucket is the level catch-up step; 3: and the fields after title
    uint16_t bucket;
    uint64_t handle;   // FieldInsHandle, the same on every game
    char name[24];
    char title[40];
    int32_t hpRow;     // its HP SpEffect (0 = none)
    int32_t defRow;    // its area defense SpEffect (0 = none)
    uint16_t flags;    // kMsgSummon: it came with another invader
    uint16_t company;  // the invader: how many summons came with it
};
static constexpr int kInvaderMsgV2Size = 80;
enum : uint16_t { kMsgSummon = 1, kMsgBack = 2 };   // kMsgBack: brought back (no banner)
// Sent before the summon: every game dresses the build's CharaInitParam row the same way
struct ArmorMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t chara;
    int32_t armor[4];
};
// Every 2 s from each game holding a copy of another game's invader (or told of one): what it sees
struct ReportMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;          // kRep*
    uint64_t handle;
    uint8_t status, update;  // the copy's ChrSetEntry there
    uint16_t dismissals;     // ghost-owner groups that game's dismissal check has dismissed so far
    int32_t hp;
    float alpha;
    uint32_t ageMs;          // since the copy appeared there (0 = no copy)
    uint32_t newsMs;         // since the announcement arrived there (0 = none)
    int32_t block;           // the copy's spot there (-1 = unknown)
    float x, y, z;
    int32_t meBlock;         // the reporting player's own spot
    float mx, my, mz;
};
// The summoner's word on a live copy of its invader that its own game no longer has (see "invaders that carry on alone")
struct GoneMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;           // kGone*
    uint64_t handle;
    uint32_t goneMs;         // since its copy left there
    uint16_t summons;        // the summoner's own summons out there (a drop takes their copies here too)
    uint16_t flags;          // kGoneDied: the summoner died; kGoneComing: they're on their way to you
};
#pragma pack(pop)
static constexpr uint32_t kReportMagic = 0x52575356;  // "VSWR"
static constexpr uint32_t kGoneMagic = 0x47575356;    // "VSWG"
enum : uint16_t { kRepInSet = 1, kRepAlive = 2, kRepGroup = 4, kRepPainted = 8, kRepAnnounced = 16 };
enum : uint16_t { kGoneYours = 1, kGoneDrop = 2 };   // it carries on alone with you; drop it (it comes back there)
enum : uint16_t { kGoneDied = 1, kGoneComing = 2 };

// The menu's view of them, and the banner
struct InvaderView {
    std::string name, title;
    int hp, maxHp;
};
static std::vector<InvaderView> s_InvaderView;
static std::string s_BannerLine, s_BannerTitle, s_RewardText;
static ULONGLONG s_BannerAt = 0, s_RewardAt = 0;

// Executable memory within reach of a rel32 call from `from`
static uint8_t* AllocNear(uintptr_t from, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t a = (from & ~(gran - 1)) - gran; a > from - 0x7ff00000; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

// What the game's name lookup gives NpcParam row `npc` from now on
static void SetInvaderName(int32_t npc, const std::string& name) {
    if (npc < kInvaderNpcFirst || npc > kInvaderNpcLast) return;
    wchar_t text[48] = {};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, text, 47);
    NameSlot* slot = nullptr;
    for (NameSlot& s : s_NameSlots)
        if (s.id.load(std::memory_order_acquire) == npc) slot = &s;
    if (slot && !wcscmp(slot->text, text)) return;
    for (NameSlot& s : s_NameSlots)
        if (!slot && !s.id.load(std::memory_order_acquire)) slot = &s;
    if (!slot) slot = &s_NameSlots[s_NextNameSlot++ % (int)(sizeof(s_NameSlots) / sizeof(s_NameSlots[0]))];
    slot->id.store(0, std::memory_order_release);
    memcpy(slot->text, text, sizeof(text));
    slot->id.store(npc, std::memory_order_release);
}

static const wchar_t* HkMsgLookup(void* repo, uint32_t version, uint32_t category, int32_t id) {
    const wchar_t* text = s_OrigMsgLookup(repo, version, category, id);
    if (text || id < kInvaderNpcFirst || id > kInvaderNpcLast) return text;
    for (NameSlot& s : s_NameSlots) {
        if (s.id.load(std::memory_order_acquire) != id) continue;
        if (category < 1024) {
            uint64_t bit = 1ull << (category & 63);
            if (!(s_NameCategories[category >> 6].fetch_or(bit) & bit)) s_NewNameCategory.store(category + 1);
        }
        return s.text;
    }
    return nullptr;
}

// Once in the world (Seamless Co-op has hooked the lookup by then): our names go in front
static void HookNames() {
    s_NameHookTried = true;
    uint8_t* site = reinterpret_cast<uint8_t*>(s_Base + MSG_LOOKUP);
    bool seamless = site[0] == 0xE9 && !memcmp(site + 5, kMsgLookupBytes + 5, sizeof(kMsgLookupBytes) - 5);
    if (!seamless && memcmp(site, kMsgLookupBytes, sizeof(kMsgLookupBytes)) != 0) {
        Log("CoopResync: invasions: the text lookup is not as expected; invaders keep the game's names");
        return;
    }
    MH_STATUS st = MH_CreateHook(site, reinterpret_cast<void*>(&HkMsgLookup), reinterpret_cast<void**>(&s_OrigMsgLookup));
    if (st == MH_OK) st = MH_EnableHook(site);
    Log("CoopResync: invasions: name lookup hook %d (%s)", st, seamless ? "after Seamless Co-op's" : "first");
}

// ---- invasion diagnostics ----
// A guest sometimes doesn't see the invader the host fights (2026-10-27: the banner came, the character
// didn't). To see where it goes, every game logs its summon groups when they change, each invader copy's
// life here (appeared, every 4 s, gone), each group the dismissal check dismisses while it holds an
// invader (the game then removes it on this game only), and an announced invader that never shows up.
// Every game also reports its copy of another game's invader to the others over the link every 2 s
// ("VSWR"), so the summoner's log has every side. The lines start "invasions/diag:".

static constexpr ULONGLONG kStatusEveryMs = 4000, kReportEveryMs = 2000;
static std::atomic<uint32_t> s_GhostDismissals{ 0 };
static std::atomic<int> s_DiagLines{ 0 };  // lines from the game thread's dismissal check, capped
static std::string s_GroupsSeen;
static int s_GroupLogs = 0, s_ReportLogs = 0;

// SummonBuddyManager ([WorldChrMan+0x1e538]) +0x70: groups by owner number (a std::map, head +0x78;
// node: children +0/+0x10, nil flag +0x19, key +0x20, list head +0x30). List nodes hold a
// SummonBuddyGroup at +0x10: chr +0x10, BuddyParam +0x1c, disappear requested +0x31, made on another
// game's request +0x3b (fromsoftware-rs; matches the dismissal check 0x4b94b0)
struct GroupEntry {
    int32_t owner;
    uintptr_t chr;
    int32_t buddy;
    bool leaving, remote;
};

static void CollectGroups(uintptr_t node, uintptr_t head, int depth, std::vector<GroupEntry>& out) {
    if (!node || node == head || depth > 32 || out.size() > 96 || Rd<uint8_t>(node + 0x19)) return;
    CollectGroups(Ptr(node), head, depth + 1, out);
    int32_t owner = Rd<int32_t>(node + 0x20);
    uintptr_t list = Ptr(node + 0x30);
    uintptr_t e = list ? Ptr(list) : 0;
    for (int i = 0; e && e != list && i < 32; i++, e = Ptr(e))
        out.push_back({ owner, Ptr(e + 0x10), Rd<int32_t>(e + 0x1c), Rd<uint8_t>(e + 0x31) != 0, Rd<uint8_t>(e + 0x3b) != 0 });
    CollectGroups(Ptr(node + 0x10), head, depth + 1, out);
}

static std::vector<GroupEntry> BuddyGroups() {
    std::vector<GroupEntry> out;
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    uintptr_t man = wcm ? Ptr(wcm + 0x1e538) : 0;
    uintptr_t head = man ? Ptr(man + 0x78) : 0;
    if (head) CollectGroups(Ptr(head + 8), head, 0, out);
    return out;
}

static std::string GroupsText(const std::vector<GroupEntry>& groups, int owner = 0) {
    std::string s;
    for (const GroupEntry& g : groups) {
        if (owner && g.owner != owner) continue;
        char buf[200];
        snprintf(buf, sizeof(buf), "%s[owner %d: buddy %d, chr %llX handle %llX npc %d hp %d%s%s]", s.empty() ? "" : " ", g.owner,
                 g.buddy, (unsigned long long)g.chr, g.chr ? (unsigned long long)Rd<uint64_t>(g.chr + 8) : 0ull,
                 g.chr ? Rd<int32_t>(g.chr + 0x60) : 0, g.chr ? Hp(g.chr) : 0, g.remote ? ", remote" : "", g.leaving ? ", leaving" : "");
        s += buf;
    }
    return s.empty() ? "none" : s;
}

static std::string SummonsOutText() { return GroupsText(BuddyGroups()); }

static bool HoldsInvader(const std::vector<GroupEntry>& groups, int owner, bool* alive) {
    bool any = false;
    for (const GroupEntry& g : groups) {
        if (g.owner != owner || !g.chr) continue;
        int32_t npc = Rd<int32_t>(g.chr + 0x60);
        if (npc < kInvaderNpcFirst || npc > kInvaderNpcLast) continue;
        any = true;
        if (Hp(g.chr) > 0) *alive = true;
    }
    return any;
}

// WorldChrMan+0x1ded8: the ChrSet of each handle container (selector bits 20-27; the entry is bits
// 0-19). Layout from fromsoftware-rs (it puts the main player at +0x1e508, as we know it); the first
// invader's log line says whether it finds that invader's own copy
static constexpr uintptr_t WCM_CHR_SETS = 0x1ded8;
static uintptr_t ChrByHandle(uint64_t handle) {
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    uint32_t sel = static_cast<uint32_t>(handle), container = (sel >> 20) & 0xff, index = sel & 0xfffff;
    if (!wcm || container >= 196) return 0;
    uintptr_t set = Ptr(wcm + WCM_CHR_SETS + container * 8);
    uintptr_t entries = set ? Ptr(set + 0x18) : 0;
    if (!entries || index >= Rd<uint32_t>(set + 0x10)) return 0;
    uintptr_t chr = Ptr(entries + index * 16);
    return chr && Rd<uint64_t>(chr + 8) == handle ? chr : 0;
}

static int32_t MaxHp(uintptr_t chr) {
    uintptr_t mods = Ptr(chr + 0x190);
    uintptr_t data = mods ? Ptr(mods) : 0;
    return data ? Rd<int32_t>(data + 0x13c) : 0;
}

// One copy's state: ChrSetEntry status/update, HP, alpha, distance from our player, where it is
static std::string CopyText(uintptr_t c, uintptr_t main) {
    uintptr_t e = Ptr(c + 0x10);
    float d = main ? Metres(main, c) : -1.0f;
    Spot s = { -1, 0, 0, 0, 0 };
    SafeSpotOf(c, s);
    char buf[300];
    snprintf(buf, sizeof(buf), "entry %u/%u, hp %d/%d, alpha %.2f (fade %.2f, base %.2f), %s from me, at %08X (%.1f, %.1f, %.1f), team %u, look %d",
             e ? Rd<uint8_t>(e + 8) : 0, e ? Rd<uint8_t>(e + 9) : 0, Hp(c), MaxHp(c), Alpha(c), FadeOpacity(c), Rd<float>(c + 0x24c),
             d >= 0.0f ? (std::to_string((int)d) + " m").c_str() : "? m (other block)", (uint32_t)s.block, s.x, s.y, s.z,
             Rd<uint8_t>(c + 0x6c), Rd<int32_t>(c + 0x540));
    return buf;
}

// The dismissal check asks about an owner only while its group has members, so every "absent" answer
// dismisses that group on this game: expected a few seconds after an invader died, but an invader still
// alive here vanishes from this game alone. Counts every dismissal of a group holding an invader (any
// owner: on other games an invader is filed under its summoner); the logging of other groups is the caller's
static void LogDismissal(int32_t number, const char* by, ULONGLONG last) {
    std::vector<GroupEntry> groups = BuddyGroups();
    bool alive = false;
    if (!HoldsInvader(groups, number, &alive)) return;
    s_GhostDismissals++;
    if (s_DiagLines.fetch_add(1) >= 100) return;
    ULONGLONG now = GetTickCount64();
    Log("CoopResync: invasions/diag: dismissal check: owner %d absent (%s; an invader last alive here %s): the game dismisses "
        "the group here%s: %s", number, by, last ? (std::to_string(now - last) + " ms ago").c_str() : "never",
        alive ? " WHILE IT IS ALIVE (it vanishes from this game)" : " (normal after a death)", GroupsText(groups, number).c_str());
}

// Seamless (its hook on this same call) also answers "absent" ONCE for a player who is here: a one-shot
// per-player flag (its peer entry +0x40), set when a summon of theirs was dismissed on their game, so this
// game drops its copy too. When this game had no copy then (it never came, or was gone already), the flag
// waits and takes their NEXT summon the moment it arrives, and that one's dismissal on their game sets it
// again: from then on every summon of theirs vanished here within a second (2026-10-29: the host's Dragon
// and both invaders on the guest's game, the guest's spirit ashes on the host's; the flag read live on the
// host, set with none of the guest's summons out). So an "absent" for a player who is here, while each of
// their summons here arrived within kFreshMs, is that stale flag (Seamless has cleared it in answering):
// they stay. Their live invaders stay too (an invader is filed under the summoner, who can dismiss their
// own spirit ash mid-invasion; when the summoner died the dismissal was a real one, and they carry on here
// alone, see "invaders that carry on alone"). Copies listed as dropped (s_Orphans: the summoner wants them
// back, or this player got away) take their owner's groups, whatever Seamless said. A group of only dead
// invaders goes kDeadInvaderMs after death if nothing dismissed it. When each group was first seen is kept by
// our per-frame task (TrackGroups); one it hasn't seen yet is new. If that task stopped (no look for 2 s),
// Seamless's answer stands.
static constexpr ULONGLONG kFreshMs = 1500, kDeadInvaderMs = kGhostLingerMs + 4000;
struct GroupSeen {
    uintptr_t chr;
    uint64_t handle;
    int32_t buddy;
    ULONGLONG first, deadSince;
    bool looked;   // its state 10 s on is logged
};
static SRWLOCK s_SeenLock = SRWLOCK_INIT;
static std::vector<GroupSeen> s_GroupSeen;
static ULONGLONG s_SeenAt = 0;
static std::atomic<int> s_KeptLines{ 0 }, s_AbsentLines{ 0 };

// Copies here of invaders carrying on alone that are to go (the task thread adds them, the dismissal check takes them)
struct Orphan {
    uintptr_t chr;
    uint64_t handle;
    ULONGLONG until;
};
static SRWLOCK s_OrphanLock = SRWLOCK_INIT;
static std::vector<Orphan> s_Orphans;

// Whether one of owner's groups here holds an orphan (its handle in *handle); those and expired ones are forgotten
static bool TakeOrphans(const std::vector<GroupEntry>& groups, int32_t number, ULONGLONG now, uint64_t* handle) {
    bool any = false;
    AcquireSRWLockExclusive(&s_OrphanLock);
    for (size_t i = 0; i < s_Orphans.size();) {
        const Orphan& o = s_Orphans[i];
        bool here = false;
        for (const GroupEntry& g : groups)
            here |= g.owner == number && g.chr == o.chr && Rd<uint64_t>(g.chr + 8) == o.handle;
        if (here) {
            any = true;
            *handle = o.handle;
        }
        if (here || now > o.until) s_Orphans.erase(s_Orphans.begin() + i);
        else i++;
    }
    ReleaseSRWLockExclusive(&s_OrphanLock);
    return any;
}

static GroupSeen* SeenOf(const GroupEntry& g, uint64_t handle) {
    for (GroupSeen& s : s_GroupSeen)
        if (s.chr == g.chr && s.handle == handle && s.buddy == g.buddy) return &s;
    return nullptr;
}

// Per-frame task: when each group here was first seen and since when its character is dead. Each summon's
// state 10 s after it came is logged once (whether it loaded and is drawn: 2026-10-29 an invader's spirit
// ashes stayed half-loaded with no position on the host's game, and the guest's Igris was never drawn there)
static std::atomic<int> s_LookLines{ 0 };
static void TrackGroups(const std::vector<GroupEntry>& groups, ULONGLONG now) {
    std::vector<std::pair<GroupEntry, ULONGLONG>> look;
    AcquireSRWLockExclusive(&s_SeenLock);
    std::vector<GroupSeen> next;
    for (const GroupEntry& g : groups) {
        if (!g.chr) continue;
        uint64_t handle = Rd<uint64_t>(g.chr + 8);
        GroupSeen* old = SeenOf(g, handle);
        GroupSeen s = old ? *old : GroupSeen{ g.chr, handle, g.buddy, now, 0, false };
        if (Hp(g.chr) > 0) s.deadSince = 0;
        else if (!s.deadSince) s.deadSince = now;
        if (!s.looked && now - s.first > 10000) {
            s.looked = true;
            look.push_back({ g, now - s.first });
        }
        next.push_back(s);
    }
    s_GroupSeen.swap(next);
    s_SeenAt = now;
    ReleaseSRWLockExclusive(&s_SeenLock);
    if (look.empty()) return;
    uintptr_t main = MainPlayer();
    for (const auto& [g, ms] : look) {
        if (s_LookLines.fetch_add(1) >= 150) break;
        uintptr_t e = Ptr(g.chr + 0x10);
        uint8_t status = e ? Rd<uint8_t>(e + 8) : 0;
        Log("CoopResync: summons: owner %d's summon (buddy %d, npc %d%s) %llu s on: %s%s", g.owner, g.buddy, Rd<int32_t>(g.chr + 0x60),
            g.remote ? ", from another game" : "", (unsigned long long)(ms / 1000), CopyText(g.chr, main).c_str(),
            status != 4 ? "; NOT LOADED (a loaded one is entry 4)" : Alpha(g.chr) < 0.05f ? "; INVISIBLE (alpha 0)" : "");
    }
}

// The player with that number in the player ChrSet (as Seamless's lookup finds them: PlayerGameData +8)
static uintptr_t PlayerByNumber(uintptr_t wcm, int32_t number) {
    uint32_t count = Rd<uint32_t>(wcm + 0x10ef0);
    uintptr_t entries = Ptr(wcm + 0x10ef8);
    for (uint32_t i = 0; entries && i < count && i < 16; i++) {
        uintptr_t chr = Ptr(entries + i * 16);
        uintptr_t pgd = chr ? Ptr(chr + 0x580) : 0;
        if (pgd && Rd<int32_t>(pgd + 8) == number) return chr;
    }
    return 0;
}

// Owner's groups here: the oldest age (ms since first seen; 0 for new ones), whether every one is a dead
// invader dead longer than kDeadInvaderMs, and whether the per-frame look is current
struct OwnerAge {
    ULONGLONG oldest = 0;
    bool deadInvaders = false, tracked = false;
};
static OwnerAge AgeOf(const std::vector<GroupEntry>& groups, int32_t number, ULONGLONG now) {
    OwnerAge r;
    AcquireSRWLockShared(&s_SeenLock);
    r.tracked = s_SeenAt && now - s_SeenAt < 2000;
    int members = 0, dead = 0;
    for (const GroupEntry& g : groups) {
        if (g.owner != number || !g.chr) continue;
        members++;
        GroupSeen* s = SeenOf(g, Rd<uint64_t>(g.chr + 8));
        if (s && now - s->first > r.oldest) r.oldest = now - s->first;
        int32_t npc = Rd<int32_t>(g.chr + 0x60);
        if (s && s->deadSince && now - s->deadSince > kDeadInvaderMs && npc >= kInvaderNpcFirst && npc <= kInvaderNpcLast) dead++;
    }
    ReleaseSRWLockShared(&s_SeenLock);
    r.deadInvaders = members && dead == members;
    return r;
}

// The dismissal check's lookup (game thread): the ghost owner is present while an invader of its
// group lives, and a few seconds after (the death fade; also armed by the summoner's announcements).
// The group itself is looked at: a copy of another game's invader is filed on that game when it
// arrives, and can be checked before our task has seen it alive, which used to dismiss it on that
// game alone (2026-10-27: the guest saw the banner but not the invader the host fought).
// Other owners: Seamless's answer, except for its stale one-shot "absent" (above)
static uintptr_t DismissLookup(uintptr_t wcm, int32_t number) {
    ULONGLONG last = s_LastInvaderAlive.load();
    ULONGLONG now = GetTickCount64();
    if (number == kGhostOwner) {
        bool lingering = last && now - last < kGhostLingerMs;
        bool alive = false;
        HoldsInvader(BuddyGroups(), kGhostOwner, &alive);
        if (alive) {
            s_LastInvaderAlive = now;
            if (!lingering && s_KeptLines.fetch_add(1) < 20)
                Log("CoopResync: invasions/diag: dismissal check: kept the ghost owner's group, an invader in it is alive "
                    "(the old check would have dismissed it here: invader last alive here %s)",
                    last ? (std::to_string(now - last) + " ms ago").c_str() : "never");
            return wcm;
        }
        if (lingering) return wcm;
        LogDismissal(number, "ours", last);
        return 0;
    }
    uintptr_t found = reinterpret_cast<uintptr_t (*)(uintptr_t, int32_t)>(s_PrevDismissLookup)(wcm, number);
    std::vector<GroupEntry> groups = BuddyGroups();
    uint64_t orphan = 0;
    if (TakeOrphans(groups, number, now, &orphan)) {
        if (s_AbsentLines.fetch_add(1) < 200)
            Log("CoopResync: summons: owner %d's summons dismissed here: invader %llX, which carried on here alone, is to go "
                "(Seamless said %s): %s", number, (unsigned long long)orphan, found ? "present" : "absent",
                GroupsText(groups, number).c_str());
        LogDismissal(number, "it carried on alone and is to go", last);
        return 0;
    }
    OwnerAge age = AgeOf(groups, number, now);
    if (found) {
        if (!age.deadInvaders) return found;
        if (s_AbsentLines.fetch_add(1) < 200)
            Log("CoopResync: summons: owner %d's group here holds only invaders dead over %llu s that nothing dismissed: "
                "dismissed it ourselves: %s", number, (unsigned long long)(kDeadInvaderMs / 1000), GroupsText(groups, number).c_str());
        LogDismissal(number, "ours: dead invaders", last);
        return 0;
    }
    uintptr_t player = PlayerByNumber(wcm, number);
    bool alive = false;
    HoldsInvader(groups, number, &alive);
    bool fresh = player && age.tracked && age.oldest < kFreshMs;
    if (player && (fresh || alive)) {
        if (s_AbsentLines.fetch_add(1) < 200)
            Log("CoopResync: summons: Seamless said owner %d is absent but they're here: kept their summons (%s): %s", number,
                fresh ? ("they arrived " + std::to_string(age.oldest) + " ms ago, so its one-shot dismissal was a stale one, "
                         "meant for an earlier summon this game no longer had").c_str()
                      : "one is a live invader",
                GroupsText(groups, number).c_str());
        return player;
    }
    if (s_AbsentLines.fetch_add(1) < 200)
        Log("CoopResync: summons: owner %d's summons dismissed here (%s, after %llu ms here): %s", number,
            !player ? "that player isn't in this game"
                    : !age.tracked ? "Seamless's one-shot dismissal; our per-frame look isn't running, so it stands"
                                   : "Seamless's one-shot dismissal: their summon was dismissed on their game",
            (unsigned long long)age.oldest, GroupsText(groups, number).c_str());
    LogDismissal(number, player ? "Seamless's one-shot dismissal" : "the player lookup", last);
    return 0;
}

// Point the dismissal check's call at DismissLookup (through a stub within rel32 reach)
static bool ChainDismissCheck() {
    uint8_t* site = reinterpret_cast<uint8_t*>(s_Base + DISMISS_CALL);
    static const uint8_t after[] = { 0x48, 0x85, 0xc0, 0x75, 0x47 };
    if (site[0] != 0xE8 || memcmp(site + 5, after, sizeof(after)) != 0) return false;
    s_PrevDismissLookup = reinterpret_cast<uintptr_t>(site) + 5 + *reinterpret_cast<int32_t*>(site + 1);
    uint8_t* stub = AllocNear(reinterpret_cast<uintptr_t>(site), 64);
    if (!stub) return false;
    uint8_t code[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };  // mov rax, DismissLookup; jmp rax
    uintptr_t target = reinterpret_cast<uintptr_t>(&DismissLookup);
    memcpy(code + 2, &target, 8);
    memcpy(stub, code, sizeof(code));
    int64_t rel = reinterpret_cast<int64_t>(stub) - (reinterpret_cast<int64_t>(site) + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    DWORD prot;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot)) return false;
    int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(site + 1, &rel32, 4);
    VirtualProtect(site, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("CoopResync: invasions: dismissal check chained (was %llX)", (unsigned long long)s_PrevDismissLookup);
    return true;
}

// Invaders.txt: <buddy> <npc> <level from> <to> <style> | <title>;  InvaderNames.txt: one per line
static void LoadInvaders() {
    char path[MAX_PATH];
    char line[512];
    GameTextures::PathNextToDll("Invaders.txt", path, sizeof(path));
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            InvaderEntry e;
            char style[64] = {};
            int used = 0;
            if (sscanf(line, "scaling %d %d %d", &s_ScaleBase, &s_ScaleStep, &s_ScaleLast) == 3) continue;
            AreaEntry area;
            int from = 0;
            int32_t npc = 0, plain = 0;
            float ref = 0;
            if (sscanf(line, "hpladder %d %f %f %d", &s_LadderBase, &s_LadderFirst, &s_LadderRatio, &s_LadderCount) == 4) continue;
            if (sscanf(line, "area %d %f %d", &area.plain, &area.hp, &area.defRow) == 3) {
                s_Areas.push_back(area);
                continue;
            }
            if (sscanf(line, "arealevel %d %d", &from, &plain) == 2) {
                s_AreaByLevel.push_back({ from, plain });
                continue;
            }
            if (sscanf(line, "hpref %d %f", &npc, &ref) == 2) {
                s_HpRef.push_back({ npc, ref });
                continue;
            }
            AshEntry ash;
            char npcs[256] = {};
            if (sscanf(line, "ash %d %d %255[0-9,] | %n", &ash.trigger, &ash.levelMin, npcs, &used) == 3 && used) {
                for (char* p = npcs; *p;) {
                    ash.npcs.push_back(atoi(p));
                    while (*p && *p != ',') p++;
                    if (*p == ',') p++;
                }
                ash.name = line + used;
                while (!ash.name.empty() && (ash.name.back() == '\n' || ash.name.back() == '\r')) ash.name.pop_back();
                s_Ashes.push_back(ash);
                continue;
            }
            used = 0;
            if (line[0] == '#' || sscanf(line, "%d %d %d %d %d %d %63s | %n", &e.buddy, &e.npc, &e.chara, &e.levelMin,
                                         &e.levelMax, &e.level, style, &used) < 7 || !used)
                continue;
            e.style = style;
            e.title = line + used;
            while (!e.title.empty() && (e.title.back() == '\n' || e.title.back() == '\r')) e.title.pop_back();
            s_InvaderList.push_back(e);
        }
        fclose(f);
    }
    GameTextures::PathNextToDll("InvaderNames.txt", path, sizeof(path));
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            std::string n = line;
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            if (!n.empty() && n[0] != '#') s_InvaderNames.push_back(n);
        }
        fclose(f);
    }
    GameTextures::PathNextToDll("InvaderArmor.txt", path, sizeof(path));
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            int slot = -1, id = 0, used = 0;
            if (line[0] == '#' || sscanf(line, "%d %d %n", &slot, &id, &used) < 2 || slot < 0 || slot > 3) continue;
            std::string name = line + used;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            s_Armor[slot].push_back({ id, name });
        }
        fclose(f);
    }
    GameTextures::PathNextToDll("InvaderLoot.txt", path, sizeof(path));
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            char kind[16] = {};
            unsigned id = 0;
            int count = 1, used = 0;
            if (line[0] == '#' || sscanf(line, "%15s %u %d %n", kind, &id, &count, &used) < 3 || !used) continue;
            uint32_t bits = !strcmp(kind, "armor") ? 0x10000000u : !strcmp(kind, "talisman") ? 0x20000000u
                          : !strcmp(kind, "goods") ? 0x40000000u : !strcmp(kind, "ash") ? 0x80000000u : 0u;
            std::string name = line + used;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            s_Loot.push_back({ bits | id, count, name });
        }
        fclose(f);
    }
    GameTextures::PathNextToDll("Invasions.ini", path, sizeof(path));
    if (FILE* f = fopen(path, "r")) {
        while (fgets(line, sizeof(line), f)) {
            char key[64];
            float v;
            if (sscanf(line, " %63[a-z_] = %f", key, &v) != 2) continue;
            if (!strcmp(key, "auto")) s_Inv.autoOn = v != 0;
            else if (!strcmp(key, "check_seconds")) s_Inv.checkSeconds = (int)v;
            else if (!strcmp(key, "guarantee_minutes")) s_Inv.guaranteeMinutes = v;
            else if (!strcmp(key, "ramp_power")) s_Inv.rampPower = v;
            else if (!strcmp(key, "loot_chance")) s_Inv.lootChance = (int)v;
            else if (!strcmp(key, "arrive_min")) s_Inv.arriveMin = v;
            else if (!strcmp(key, "arrive_max")) s_Inv.arriveMax = v;
            else if (!strcmp(key, "look")) s_Inv.look = (int)v;
            else if (!strcmp(key, "random_armor")) s_Inv.randomArmor = v != 0;
            else if (!strcmp(key, "level_margin")) s_Inv.levelMargin = (int)v;
            else if (!strcmp(key, "summons_1")) s_Inv.summons[0] = (int)v;
            else if (!strcmp(key, "summons_2")) s_Inv.summons[1] = (int)v;
            else if (!strcmp(key, "summons_3")) s_Inv.summons[2] = (int)v;
            else if (!strcmp(key, "summon_tarnished")) s_Inv.summonTarnished = (int)v;
            else if (!strcmp(key, "summon_ashes")) s_Inv.summonAshes = v != 0;
        }
        fclose(f);
    }
    Log("CoopResync: invasions: %zu invaders, %zu names, %zu loot items, armor %zu/%zu/%zu/%zu; auto %d every %ds, certain "
        "after %.0f min (power %.1f), loot %d%%, arrive %.0f-%.0f m, look %d, random armor %d, level margin %d, scaling %d "
        "step %d to %d", s_InvaderList.size(), s_InvaderNames.size(), s_Loot.size(), s_Armor[0].size(), s_Armor[1].size(),
        s_Armor[2].size(), s_Armor[3].size(), s_Inv.autoOn, s_Inv.checkSeconds, s_Inv.guaranteeMinutes, s_Inv.rampPower,
        s_Inv.lootChance, s_Inv.arriveMin, s_Inv.arriveMax, s_Inv.look, s_Inv.randomArmor, s_Inv.levelMargin, s_ScaleBase,
        s_ScaleStep, s_ScaleLast);
    Log("CoopResync: invasions: HP like the game's NPC invaders: %zu areas (%zu by level), %zu HP references, %d HP SpEffects from "
        "%d; summons 1/2/3: %d%%/%d%%/%d%%, %d%% of them Tarnished, %zu spirit ashes (%s)", s_Areas.size(), s_AreaByLevel.size(),
        s_HpRef.size(), s_LadderCount, s_LadderBase, s_Inv.summons[0], s_Inv.summons[1], s_Inv.summons[2], s_Inv.summonTarnished,
        s_Ashes.size(), s_Inv.summonAshes ? "on" : "off: summons are all Tarnished");
    for (const InvaderEntry& e : s_InvaderList) SetInvaderName(e.npc, e.title);
    for (const AshEntry& a : s_Ashes)
        for (int32_t npc : a.npcs) SetInvaderName(npc, a.name);
}

static void ShowReward(const std::string& text) {
    std::lock_guard<std::mutex> lock(s_Lock);
    s_RewardText = text;
    s_RewardAt = GetTickCount64();
}

// The host's path, every half second, leaving out samples that rose or fell more than 1.5 m since the one
// before (jumping, falling, riding a lift)
static void RecordTrail(uintptr_t main, ULONGLONG now) {
    if (now < s_NextTrail || !main || Hp(main) <= 0) return;
    s_NextTrail = now + 500;
    TrailPoint t = { Rd<uint32_t>(main + 0x6d0), {}, now };
    for (int i = 0; i < 3; i++) t.p[i] = Rd<float>(main + 0x6c0 + 4 * i);
    t.p[3] = 1.0f;
    bool steady = s_LastSample.at && s_LastSample.block == t.block && fabsf(s_LastSample.p[1] - t.p[1]) <= 1.5f;
    s_LastSample = t;
    if (!steady) return;
    s_Trail.push_back(t);
    if (s_Trail.size() > 1200) s_Trail.erase(s_Trail.begin());   // ten minutes
}

// Where invaders arrive: ground the host walked on (same block, at least 3 s ago), as far as possible within
// arriveMin-arriveMax metres of the host and at least 15 m from every player; failing that the farthest such spot
// at least 15 m out. Returns the distance from the host (0 = none: right here). Their AI senses the nearest player
// from anywhere (generate.py's NpcThinkParam clones), so they come looking.
static float ArrivalPoint(uintptr_t main, ULONGLONG now, float out[4]) {
    alignas(16) uint8_t buf[0x20] = {};
    const float* phys = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(main, buf);
    if (!phys) return 0;
    uint32_t block = Rd<uint32_t>(main + 0x6d0);
    float p[3], offset[3];   // the host's map position; physics = map + offset on this game right now
    for (int i = 0; i < 3; i++) {
        p[i] = Rd<float>(main + 0x6c0 + 4 * i);
        offset[i] = phys[i] - p[i];
    }
    std::vector<std::pair<float, float>> players = { { p[0], p[2] } };
    if (uintptr_t wcm = Global(WORLD_CHR_MAN)) {
        uint32_t cap = Rd<uint32_t>(wcm + 0x10ee0 + 0x10);
        uintptr_t entries = Ptr(wcm + 0x10ee0 + 0x18);
        for (uint32_t i = 0; entries && i < cap && i < 16; i++) {
            uintptr_t c = Ptr(entries + i * 16);
            if (!c || c == main || Rd<uint32_t>(c + 0x30) != block) continue;
            alignas(16) uint8_t cb[0x20] = {};
            if (const float* q = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(c, cb))
                players.push_back({ q[0] - offset[0], q[2] - offset[2] });
        }
    }
    const TrailPoint* best = nullptr;
    const TrailPoint* fallback = nullptr;
    float bestD = 0, fallbackD = 0;
    for (const TrailPoint& t : s_Trail) {
        if (t.block != block || now - t.at < 3000) continue;
        bool clear = true;
        for (const auto& pl : players) {
            float dx = t.p[0] - pl.first, dz = t.p[2] - pl.second;
            clear &= dx * dx + dz * dz >= 15.0f * 15.0f;
        }
        if (!clear) continue;
        float dx = t.p[0] - p[0], dz = t.p[2] - p[2], d = sqrtf(dx * dx + dz * dz);
        if (d >= s_Inv.arriveMin && d <= s_Inv.arriveMax && d > bestD) best = &t, bestD = d;
        if (d <= s_Inv.arriveMax && d > fallbackD) fallback = &t, fallbackD = d;
    }
    const TrailPoint* pick = best ? best : fallback;
    if (!pick) return 0;
    for (int i = 0; i < 3; i++) out[i] = pick->p[i] + offset[i];
    out[3] = phys[3];
    return best ? bestD : fallbackD;
}

// Roundtable Hold (m11_10), boss fights, loading, dead: no invasion now
static const char* InvasionBlocked(uintptr_t main) {
    uintptr_t gm = Global(GAME_MAN), gdm = Global(GAME_DATA_MAN);
    if (!main) return "not loaded in";
    if (Hp(main) <= 0) return "dead";
    if (gm && Rd<uint8_t>(gm + 0x10)) return "loading";
    if (gdm && Rd<uint8_t>(gdm + 0xc0)) return "boss fight";
    if ((Rd<uint32_t>(main + 0x6d0) >> 16) == 0x0B0A) return "Roundtable Hold";
    return nullptr;
}

static uint32_t Random(uint32_t n) {
    static uint64_t state = GetTickCount64() ^ reinterpret_cast<uintptr_t>(&state);
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return n ? static_cast<uint32_t>((state >> 33) % n) : 0;
}

static bool IsInvader(uintptr_t chr) {
    int32_t npc = Rd<int32_t>(chr + 0x60);
    return npc >= kInvaderNpcFirst && npc <= kInvaderNpcLast;
}

// Invader characters in the buddy ChrSet (WorldChrMan+0x10f90)
static std::vector<uintptr_t> InvaderChrs() {
    std::vector<uintptr_t> out;
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    if (!wcm) return out;
    uint32_t cap = Rd<uint32_t>(wcm + 0x10f90 + 0x10);
    uintptr_t entries = Ptr(wcm + 0x10f90 + 0x18);
    for (uint32_t i = 0; entries && i < cap && i < 1000; i++) {
        uintptr_t chr = Ptr(entries + i * 16);
        if (chr && IsInvader(chr)) out.push_back(chr);
    }
    return out;
}

static ActiveInvader* ActiveByHandle(uint64_t handle) {
    for (ActiveInvader& a : s_Active)
        if (a.handle == handle) return &a;
    return nullptr;
}

static const AshEntry* AshByNpc(int32_t npc) {
    for (const AshEntry& a : s_Ashes)
        for (int32_t n : a.npcs)
            if (n == npc) return &a;
    return nullptr;
}

// The ordinary area scaling SpEffect most enemies within 150 m of the host carry (0 = none seen). Looked at every
// 5 s in every ChrSet (WorldChrMan+0x1ded8); a SpEffect list is CSSpecialEffect ([chr+0x178]) +8, nodes {+8 id,
// +0x30 next}
static void ScanArea(uintptr_t main, ULONGLONG now) {
    if (now < s_NextAreaScan || !main || s_Areas.empty()) return;
    s_NextAreaScan = now + 5000;
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    if (!wcm) return;
    std::vector<std::pair<int32_t, int>> votes;
    int looked = 0, visited = 0;
    for (int set = 0; set < 196 && looked < 400 && visited < 8000; set++) {
        uintptr_t cs = Ptr(wcm + WCM_CHR_SETS + set * 8);
        uintptr_t entries = cs ? Ptr(cs + 0x18) : 0;
        uint32_t cap = cs ? Rd<uint32_t>(cs + 0x10) : 0;
        for (uint32_t i = 0; entries && i < cap && i < 2048 && looked < 400 && visited < 8000; i++, visited++) {
            uintptr_t c = Ptr(entries + i * 16);
            if (!c || c == main || IsInvader(c) || Hp(c) <= 0) continue;
            float d = Metres(main, c);
            if (d < 0.0f || d > 150.0f) continue;
            looked++;
            uintptr_t effects = Ptr(c + 0x178);
            uintptr_t node = effects ? Ptr(effects + 8) : 0;
            for (int n = 0; node && n < 64; n++, node = Ptr(node + 0x30)) {
                int32_t id = Rd<int32_t>(node + 8);
                bool area = false;
                for (const AreaEntry& e : s_Areas) area |= e.plain == id;
                if (!area) continue;
                bool counted = false;
                for (auto& v : votes)
                    if (v.first == id) v.second++, counted = true;
                if (!counted) votes.push_back({ id, 1 });
                break;
            }
        }
    }
    int32_t best = 0;
    int bestN = 0;
    for (const auto& v : votes)
        if (v.second > bestN) best = v.first, bestN = v.second;
    if (best && best != s_AreaSeen)
        Log("CoopResync: invasions: the area here is %d (%d of %d characters within 150 m carry it)", best, bestN, looked);
    if (best) s_AreaSeen = best;
}

// The area an invader gets its HP for: what the enemies here say, else the one for the host's level
static const AreaEntry* CurrentArea(int level, bool* seen) {
    int32_t plain = s_AreaSeen;
    *seen = plain != 0;
    if (!plain)
        for (const auto& l : s_AreaByLevel)
            if (level >= l.first) plain = l.second;
    for (const AreaEntry& e : s_Areas)
        if (e.plain == plain) return &e;
    return nullptr;
}

// Own invader, on its first frame with its HP up and nothing of ours on it: the HP SpEffect that gives it the HP the
// game's own NPC invader has in the host's area (that NPC's body, "hpref", times the area's human-NPC multiplier;
// measured against the max HP it came with), and that area's defense
static void DecideHp(ActiveInvader& a, uintptr_t c, int level) {
    a.hpDecided = true;
    bool seen = false;
    const AreaEntry* area = CurrentArea(level, &seen);
    int32_t npc = Rd<int32_t>(c + 0x60);
    float ref = 0;
    for (const auto& r : s_HpRef)
        if (r.first == npc) ref = r.second;
    int32_t base = MaxHp(c);
    if (!area || ref <= 0 || base <= 0 || !s_LadderBase || s_LadderCount <= 0 || s_LadderFirst <= 0 || s_LadderRatio <= 1.0f) {
        Log("CoopResync: invasions: %s keeps its own HP (area %s, reference %.0f, max HP %d)", a.name.c_str(),
            area ? std::to_string(area->plain).c_str() : "unknown", ref, base);
        return;
    }
    float want = ref * area->hp / base;
    int k = (int)lroundf(logf(want / s_LadderFirst) / logf(s_LadderRatio));
    k = (std::max)(0, (std::min)(k, s_LadderCount - 1));
    a.hpRow = s_LadderBase + k;
    a.defRow = area->defRow;
    Log("CoopResync: invasions: %s HP: %d of its own, an NPC invader here has %.0f (%.0f x %.2f, area %d%s): x%.2f (SpEffect %d), "
        "defense %d", a.name.c_str(), base, ref * area->hp, ref, area->hp, area->plain, seen ? " from the enemies here" : " for level",
        s_LadderFirst * powf(s_LadderRatio, (float)k), a.hpRow, a.defRow);
}

static void DiagAppeared(ActiveInvader& a, uintptr_t c, uintptr_t main, ULONGLONG now) {
    a.appeared = now;
    a.nextStatus = now + kStatusEveryMs;
    uintptr_t byHandle = ChrByHandle(a.handle);
    std::string when = a.own ? "our summon" : a.newsAt ? std::to_string(now - a.newsAt) + " ms after the announcement" : "no announcement yet";
    Log("CoopResync: invasions/diag: copy of %llX (%s) appeared here (%s): chr %llX npc %d, %s; handle lookup %s; groups: %s",
        (unsigned long long)a.handle, a.name.c_str(), when.c_str(), (unsigned long long)c, Rd<int32_t>(c + 0x60),
        CopyText(c, main).c_str(), byHandle == c ? "finds it" : byHandle ? "finds another character" : "misses",
        GroupsText(BuddyGroups()).c_str());
}

static void SendReport(const ActiveInvader& a, uintptr_t main, ULONGLONG now) {
    void* net = LinkOk() ? s_NetMessages() : nullptr;
    if (!net) return;
    ReportMsg r = {};
    r.magic = kReportMagic;
    r.version = 1;
    r.handle = a.handle;
    r.dismissals = static_cast<uint16_t>((std::min)(s_GhostDismissals.load(), 65535u));
    r.block = -1;
    r.meBlock = -1;
    if (uintptr_t c = a.chr) {
        uintptr_t e = Ptr(c + 0x10);
        r.flags |= kRepInSet | (Hp(c) > 0 ? kRepAlive : 0) | (a.painted ? kRepPainted : 0);
        r.status = e ? Rd<uint8_t>(e + 8) : 0;
        r.update = e ? Rd<uint8_t>(e + 9) : 0;
        r.hp = Hp(c);
        r.alpha = Alpha(c);
        r.ageMs = static_cast<uint32_t>(now - a.appeared);
        Spot s;
        if (SafeSpotOf(c, s)) r.block = s.block, r.x = s.x, r.y = s.y, r.z = s.z;
    }
    for (const GroupEntry& g : BuddyGroups())
        if (g.owner == kGhostOwner) r.flags |= kRepGroup;
    if (a.newsAt) {
        r.flags |= kRepAnnounced;
        r.newsMs = static_cast<uint32_t>(now - a.newsAt);
    }
    if (main) {
        r.meBlock = Rd<int32_t>(main + 0x6d0);
        r.mx = Rd<float>(main + 0x6c0), r.my = Rd<float>(main + 0x6c4), r.mz = Rd<float>(main + 0x6c8);
    }
    for (const Member& m : Members()) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        s_Send(net, &to, &r, sizeof(r), 32, kLinkChannel);
    }
}

// The big line across the screen (the invasion banner's), and a smaller one under it
static void ShowNotice(const std::string& line, const std::string& title) {
    std::lock_guard<std::mutex> lock(s_Lock);
    s_BannerLine = line;
    s_BannerTitle = title;
    s_BannerAt = GetTickCount64();
}

static std::string MemberName(uint64_t steamId) {
    for (const Member& m : Members())
        if (m.steamId == steamId) return NameOf(m, nullptr);
    return "The other player";
}

// The summoner: word on one of its invaders that carries on alone (kGoneDrop goes reliably)
static void SendGone(uint64_t to, const GoneInvader& g, uint16_t kind, ULONGLONG now) {
    void* net = LinkOk() ? s_NetMessages() : nullptr;
    if (!net) return;
    GoneMsg msg = {};
    msg.magic = kGoneMagic;
    msg.version = 1;
    msg.kind = kind;
    msg.handle = g.handle;
    msg.goneMs = static_cast<uint32_t>(now - g.at);
    msg.flags = (g.died ? kGoneDied : 0) | (g.comingTo == to ? kGoneComing : 0);
    uintptr_t main = MainPlayer();
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    int32_t number = pgd ? Rd<int32_t>(pgd + 8) : -1;
    for (const GroupEntry& e : BuddyGroups())
        if (e.owner == number) msg.summons++;
    SteamIdentity id = { 16, 8, to, {} };
    s_Send(net, &id, &msg, sizeof(msg), kind == kGoneDrop ? 8 | 32 : 32, kLinkChannel);
}

// The summoner: another game's report on one of our invaders that's gone from here. From kOrphanAfterMs on (a copy
// only between loads here isn't taken for one), every game reporting a live copy is told it carries on alone there;
// the first holds it: its reports say where it is and its HP, and that it's gone there too (no copy, or dead)
static void HearOrphan(uint64_t from, const ReportMsg& r, ULONGLONG now) {
    GoneInvader* g = nullptr;
    for (GoneInvader& o : s_GoneOwn)
        if (o.handle == r.handle) g = &o;
    if (!g || now - g->at < kOrphanAfterMs) return;
    bool holder = !g->holder || from == g->holder;
    if (!(r.flags & kRepInSet)) {
        if (holder && g->heardAt) g->dropped = true;
        return;
    }
    if (!(r.flags & kRepAlive)) {
        if (holder && g->heardAt) g->hp = 0;
        return;
    }
    if (!g->asked) SendGone(from, *g, kGoneYours, now);
    if (!holder) return;
    bool first = !g->heardAt;
    g->holder = from;
    g->heardAt = now;
    g->hp = r.hp;
    if (r.block != -1) g->spot = { r.block, r.x, r.y, r.z, 0.0f };
    if (first) {
        g->holderName = MemberName(from);
        Log("CoopResync: invasions: our invader %s (%llX) left here %llu s ago%s but carries on alone on %s's game (hp %d at %08X "
            "(%.1f, %.1f, %.1f)); it comes back when we're within %d m of it", g->was.name.c_str(), (unsigned long long)g->handle,
            (unsigned long long)((now - g->at) / 1000), g->died ? " (we died)" : "", g->holderName.c_str(), r.hp, (uint32_t)r.block,
            r.x, r.y, r.z, (int)kReturnMetres);
    }
}

// Whether invaders of ours carry on alone on another game (no new invasion meanwhile); the holder's name
static bool OrphansOut(ULONGLONG now, std::string* holder = nullptr) {
    for (const GoneInvader& g : s_GoneOwn)
        if (g.heardAt && g.hp > 0 && (g.asked || (!g.dropped && now - g.heardAt < kOrphanQuietMs))) {
            if (holder) *holder = g.holderName;
            return true;
        }
    return false;
}

// Another game: our copy of an invader carrying on alone is to go (the dismissal check takes its owner's groups)
static void DropCopy(const ActiveInvader& a, ULONGLONG now) {
    AcquireSRWLockExclusive(&s_OrphanLock);
    bool listed = false;
    for (Orphan& o : s_Orphans)
        if (o.chr == a.chr && o.handle == a.handle) {
            o.until = now + 10000;
            listed = true;
        }
    if (!listed) s_Orphans.push_back({ a.chr, a.handle, now + 10000 });
    ReleaseSRWLockExclusive(&s_OrphanLock);
}

// Another game: the summoner's word on our copy of its invader
static ULONGLONG s_AloneNewsAt = 0;
static void OnGone(uint64_t from, const GoneMsg& g, ULONGLONG now) {
    ActiveInvader* a = ActiveByHandle(g.handle);
    if (!a || a->own || !a->chr || Hp(a->chr) <= 0) return;
    if (g.kind == kGoneYours) {
        if (a->orphan) return;
        a->orphan = true;
        Log("CoopResync: invasions: %llX says %llX (%s) left its game %u s ago%s; our copy carries on here alone (hp %d)",
            (unsigned long long)from, (unsigned long long)g.handle, a->name.c_str(), g.goneMs / 1000,
            (g.flags & kGoneDied) ? " (they died)" : (g.flags & kGoneComing) ? " (they're on their way here)" : "", Hp(a->chr));
        if (now - s_AloneNewsAt > 30000) {
            s_AloneNewsAt = now;
            if (g.flags & kGoneComing)
                ShowNotice(MemberName(from) + " is on the way!", "Hold on: the invaders are still after you");
            else
                ShowNotice("You're on your own!", (g.flags & kGoneDied) ? MemberName(from) + " died: the invaders are still after you"
                                                                       : std::string("The invaders are still after you"));
        }
        return;
    }
    if (g.kind != kGoneDrop) return;
    a->orphan = true;
    DropCopy(*a, now);
    if (a->goneLines++ < 3)
        Log("CoopResync: invasions: %llX wants %llX (%s) back (hp %d here): dropping our copy%s", (unsigned long long)from,
            (unsigned long long)g.handle, a->name.c_str(), Hp(a->chr),
            g.summons ? (" (their " + std::to_string(g.summons) + " summon(s) here go with it)").c_str() : "");
}

static void OnReport(uint64_t from, const ReportMsg& r, ULONGLONG now) {
    if (r.version != 1) return;
    HearOrphan(from, r, now);
    if (s_ReportLogs >= 600) return;
    s_ReportLogs++;
    ActiveInvader* a = ActiveByHandle(r.handle);
    Spot there = { r.block, r.x, r.y, r.z, 0.0f }, them = { r.meBlock, r.mx, r.my, r.mz, 0.0f }, here = { -1, 0, 0, 0, 0 };
    if (a && a->chr) SafeSpotOf(a->chr, here);
    float apart = r.block != -1 && here.block != -1 ? SpotMetres(there, here) : -1.0f;
    float fromThem = r.block != -1 ? SpotMetres(there, them) : -1.0f;
    char copy[200];
    if (r.flags & kRepInSet)
        snprintf(copy, sizeof(copy), "their copy for %u s: entry %u/%u, hp %d, alpha %.2f, %s, %s from them, %s from ours", r.ageMs / 1000,
                 r.status, r.update, r.hp, r.alpha, (r.flags & kRepPainted) ? "painted" : "NOT painted",
                 fromThem >= 0.0f ? (std::to_string((int)fromThem) + " m").c_str() : "? m",
                 apart >= 0.0f ? (std::to_string((int)apart) + " m").c_str() : "? m");
    else
        snprintf(copy, sizeof(copy), "NO copy in their buddy set");
    Log("CoopResync: invasions/diag: %llX reports %llX (%s): %s; announcement %s; owner-%d group there %s; invader groups dismissed "
        "there so far %u; they're at %08X (%.1f, %.1f, %.1f)", (unsigned long long)from, (unsigned long long)r.handle,
        a ? a->name.c_str() : "not known here", copy, (r.flags & kRepAnnounced) ? (std::to_string(r.newsMs / 1000) + " s ago").c_str() : "not had",
        kGhostOwner, (r.flags & kRepGroup) ? "yes" : "no", r.dismissals, (uint32_t)r.meBlock, r.mx, r.my, r.mz);
}

static std::string CompanyTitle(const std::string& title, int company) {
    if (!company) return title;
    return title + (title.empty() ? "" : ", ") + "with " + std::to_string(company) + (company == 1 ? " summon" : " summons");
}

static void ShowBanner(const std::string& name, const std::string& title) { ShowNotice("Invaded by " + name + "!", title); }

static void SendInvader(const ActiveInvader& a) {
    void* net = LinkOk() ? s_NetMessages() : nullptr;
    if (!net) return;
    InvaderMsg msg = {};
    msg.magic = kInvaderMagic;
    msg.version = 3;
    msg.bucket = static_cast<uint16_t>(a.bucket < 0 ? 0 : a.bucket);
    msg.handle = a.handle;
    strncpy(msg.name, a.name.c_str(), sizeof(msg.name) - 1);
    strncpy(msg.title, a.title.c_str(), sizeof(msg.title) - 1);
    msg.hpRow = a.hpRow;
    msg.defRow = a.defRow;
    msg.flags = (a.summon ? kMsgSummon : 0) | (a.back ? kMsgBack : 0);
    msg.company = static_cast<uint16_t>(a.company);
    for (const Member& m : Members()) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        s_Send(net, &to, &msg, sizeof(msg), 32, kLinkChannel);
    }
}

// A build's CharaInitParam row in this game's memory (0 if the table isn't as expected)
static uintptr_t CharaRow(int32_t id) {
    uintptr_t file = ParamFile(kCharaInitTable, L"CharaInitParam");
    if (!file) return 0;
    int rows = Rd<uint16_t>(file + 0xa);
    for (int lo = 0, hi = rows - 1; lo <= hi;) {
        int mid = (lo + hi) / 2;
        int32_t rid = Rd<int32_t>(file + 0x40 + mid * 0x18);
        if (rid == id) return file + Rd<uint64_t>(file + 0x48 + mid * 0x18);
        if (rid < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static bool DressRow(int32_t chara, const int32_t armor[4]) {
    uintptr_t row = CharaRow(chara);
    if (!row) return false;
    for (int i = 0; i < 4; i++) *reinterpret_cast<volatile int32_t*>(row + kArmorFields[i]) = armor[i];
    return true;
}

static void SendArmor(int32_t chara, const int32_t armor[4]) {
    void* net = LinkOk() ? s_NetMessages() : nullptr;
    if (!net) return;
    ArmorMsg msg = {};
    msg.magic = kArmorMagic;
    msg.version = 1;
    msg.chara = chara;
    memcpy(msg.armor, armor, sizeof(msg.armor));
    for (const Member& m : Members()) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        s_Send(net, &to, &msg, sizeof(msg), 8 | 32, kLinkChannel);   // reliable
    }
}

static void OnLinkMessage(uint64_t from, const uint8_t* data, int size, ULONGLONG now) {
    if (size == (int)sizeof(HorseMsg) && Rd<uint32_t>(reinterpret_cast<uintptr_t>(data)) == kHorseMagic) {
        HorseMsg h;
        memcpy(&h, data, sizeof(h));
        h.song[sizeof(h.song) - 1] = 0;
        if (h.version == 1) HorseUp::Heard(from, h.song);
        return;
    }
    if (size == (int)sizeof(ReportMsg) && Rd<uint32_t>(reinterpret_cast<uintptr_t>(data)) == kReportMagic) {
        ReportMsg r;
        memcpy(&r, data, sizeof(r));
        OnReport(from, r, now);
        return;
    }
    if (size == (int)sizeof(GoneMsg) && Rd<uint32_t>(reinterpret_cast<uintptr_t>(data)) == kGoneMagic) {
        GoneMsg g;
        memcpy(&g, data, sizeof(g));
        if (g.version == 1) OnGone(from, g, now);
        return;
    }
    if (size == (int)sizeof(ArmorMsg)) {
        ArmorMsg am;
        memcpy(&am, data, sizeof(am));
        if (am.magic != kArmorMagic || am.version != 1) return;
        s_LastInvaderAlive = now;  // the summon follows in a moment: its group must not be dismissed on arrival
        bool ok = am.chara >= 3910000 && am.chara <= 3919999 && DressRow(am.chara, am.armor);
        Log("CoopResync: invasions: %llX dresses %d in %d/%d/%d/%d: %s", (unsigned long long)from, am.chara, am.armor[0],
            am.armor[1], am.armor[2], am.armor[3], ok ? "done" : "NOT done");
        return;
    }
    InvaderMsg msg = {};
    if (size != (int)sizeof(msg) && size != kInvaderMsgV2Size) return;
    memcpy(&msg, data, size);
    if (msg.magic != kInvaderMagic || msg.version < 1 || msg.version > 3 || (msg.version == 3) != (size == (int)sizeof(msg)))
        return;
    msg.name[sizeof(msg.name) - 1] = 0;
    msg.title[sizeof(msg.title) - 1] = 0;
    ActiveInvader* a = ActiveByHandle(msg.handle);
    if (a && a->named && !a->ash) return;
    if (!a) {
        s_Active.push_back({ msg.handle, "", "", now, 0, 0 });
        a = &s_Active.back();
    }
    a->name = msg.name;
    a->title = msg.title;
    a->named = true;
    a->ash = false;
    a->bucket = msg.version >= 2 ? msg.bucket : 0;
    a->hpRow = msg.hpRow;
    a->defRow = msg.defRow;
    a->summon = (msg.flags & kMsgSummon) != 0;
    a->company = msg.company;
    if (!a->newsAt) {
        a->newsAt = now;
        s_LastInvaderAlive = now;  // as for the armor: a copy arriving now stays
    }
    Log("CoopResync: invasions: %llX says %llX is %s (%s), level step %d, HP SpEffect %d, defense %d%s; %s", (unsigned long long)from,
        (unsigned long long)msg.handle, msg.name, msg.title, a->bucket, a->hpRow, a->defRow,
        a->summon ? ", a summon" : a->company ? (", with " + std::to_string(a->company) + " summon(s)").c_str() : "",
        a->chr ? "its copy is here" : ChrByHandle(msg.handle) ? "its handle finds a character here, not in the buddy set" : "no copy here yet");
    if (a->summon) return;
    if (msg.flags & kMsgBack) ShowNotice(MemberName(from) + " is back!", "The invaders are on both of you again");
    else ShowBanner(msg.name, CompanyTitle(msg.title, a->company));
}

static bool CallSummon(uintptr_t man) {
    __try {
        reinterpret_cast<void (*)(uintptr_t)>(Addr(SUMMON_REQUEST))(man);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Host (or solo): pick one invader for the host's level and maybe 1-3 summons to come with it (other Tarnished or
// spirit ashes, Invasions.ini summons_*), and dress each Tarnished's row in random armor on every game (the others are
// told first and the summon waits a moment, as each game builds its copy from its own row)
static void SummonInvader(ULONGLONG now, bool automatic) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer(), wcm = Global(WORLD_CHR_MAN);
    uintptr_t man = wcm ? Ptr(wcm + 0x1e538) : 0;
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    if (!s_InvasionsOk || !man || !pgd || !Intact(SUMMON_REQUEST, true)) return SetMessage("Invasions aren't available");
    if (lobby == 6) return SetMessage("Only the host can summon invaders");
    std::string holder;
    if (OrphansOut(now, &holder)) return SetMessage("Your last invaders are still hunting " + holder);
    if (s_Pending.on) {
        if (automatic || !s_Pending.automatic || !s_Pending.tries) return SetMessage("An invader is on the way");
        Log("CoopResync: invasions: the automatic invasion waiting for ground to arrive on makes way for this one");
        s_Pending = {};
    }
    if (const char* why = InvasionBlocked(main)) return SetMessage(std::string("No invasion now: ") + why);
    int level = Rd<int32_t>(pgd + 0x68);
    std::vector<const InvaderEntry*> fit;
    for (const InvaderEntry& e : s_InvaderList)
        if (level >= e.levelMin && level <= e.levelMax && e.level <= level + s_Inv.levelMargin) fit.push_back(&e);
    if (fit.empty()) return SetMessage("No invader fits your level");
    std::vector<const AshEntry*> ashes;
    for (const AshEntry& a : s_Ashes)
        if (s_Inv.summonAshes && level >= a.levelMin) ashes.push_back(&a);
    int roll = (int)Random(100), company = 0, edge = 0;
    for (int n = 3; n >= 1; n--) {
        edge += s_Inv.summons[n - 1];
        if (!company && roll < edge) company = n;
    }
    s_Pending = {};
    s_Pending.on = true;
    s_Pending.automatic = automatic;
    s_Pending.at = now;
    // each Tarnished a different build (each has its own CharaInitParam row to dress) and a different name
    auto addTarnished = [&]() {
        size_t i = Random((uint32_t)fit.size());
        PendingMember m;
        m.pick = fit[i];
        fit.erase(fit.begin() + i);
        for (int tries = 0; tries < 10; tries++) {
            m.name = s_InvaderNames.empty() ? m.pick->title : s_InvaderNames[Random((uint32_t)s_InvaderNames.size())];
            bool taken = false;
            for (const PendingMember& o : s_Pending.members) taken |= o.name == m.name;
            if (!taken) break;
        }
        m.bucket = s_ScaleStep > 0 ? (std::min)((std::max)(level - m.pick->level, 0) / s_ScaleStep, s_ScaleLast) : 0;
        s_Pending.members.push_back(m);
    };
    addTarnished();
    for (int n = 0; n < company; n++) {
        bool tarnished = (int)Random(100) < s_Inv.summonTarnished;
        if (tarnished ? fit.empty() : ashes.empty()) tarnished = !tarnished;
        if (tarnished && !fit.empty()) {
            addTarnished();
        } else if (!tarnished && !ashes.empty()) {
            PendingMember m;
            m.ash = ashes[Random((uint32_t)ashes.size())];
            m.name = m.ash->name;
            s_Pending.members.push_back(m);
        }
    }
    bool others = InSession(lobby) && Members().size() > 1;
    if (s_Inv.randomArmor && s_Armor[0].size() && s_Armor[1].size() && s_Armor[2].size() && s_Armor[3].size()) {
        for (PendingMember& m : s_Pending.members) {
            if (!m.pick) continue;
            int32_t* armor = m.armor;
            std::string names;
            for (int i = 0; i < 4; i++) {
                const ArmorPiece& a = s_Armor[i][Random((uint32_t)s_Armor[i].size())];
                armor[i] = a.id;
                names += (i ? ", " : "") + a.name;
            }
            bool dressed = m.dressed = DressRow(m.pick->chara, armor);
            if (others) SendArmor(m.pick->chara, armor);
            Log("CoopResync: invasions: %s dressed in %s%s", m.pick->title.c_str(), names.c_str(), dressed ? "" : " (row NOT found)");
        }
        if (others) s_Pending.at = now + kArmorLeadMs;
    }
}

// Spirit ash upgrade for the host's level: +0 at 10, +10 at 140
static int AshUpgrade(int level) { return (std::max)(0, (std::min)((level - 10) / 13, 10)); }

// Where a spot is in this game's physics frame, from the main player's own (same block, or the same open world)
static bool PhysicsAt(uintptr_t main, const Spot& s, float out[3]) {
    Spot me;
    alignas(16) uint8_t buf[0x20] = {};
    const float* phys = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(main, buf);
    if (!phys || s.block == -1 || !SafeSpotOf(main, me)) return false;
    float pa[3] = { s.x, s.y, s.z }, pb[3] = { me.x, me.y, me.z };
    if (s.block != me.block) {
        uint32_t wa = 0, wb = 0;
        if (!WorldPos(s, wa, pa) || !WorldPos(me, wb, pb) || wa != wb) return false;
    }
    for (int i = 0; i < 3; i++) out[i] = phys[i] + pa[i] - pb[i];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

static constexpr ULONGLONG kArriveRetryMs = 20000;

static void SummonPending(ULONGLONG now) {
    PendingInvasion p = s_Pending;
    s_Pending = {};
    uintptr_t main = MainPlayer(), wcm = Global(WORLD_CHR_MAN);
    uintptr_t man = wcm ? Ptr(wcm + 0x1e538) : 0;
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    if (!man || !pgd) return SetMessage("Invasions aren't available");
    if (const char* why = InvasionBlocked(main)) {
        // invaders coming back are already gone from the other game: they wait a few seconds for the host
        if (p.back && p.tries < 5) {
            p.tries++;
            p.at = now + 1000;
            s_Pending = p;
            return;
        }
        if (p.back) return Log("CoopResync: invasions: our invaders couldn't come back (%s): they're gone", why);
        // an automatic one stays a surprise: the roll comes round again
        if (p.automatic) return Log("CoopResync: invasions: an automatic invasion is due but waits (%s)", why);
        return SetMessage(std::string("No invasion now: ") + why);
    }
    if (p.members.empty()) return;
    bool automatic = p.automatic;
    int level = Rd<int32_t>(pgd + 0x68);

    alignas(16) uint8_t buf[0x20] = {};
    const float* phys = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(main, buf);
    alignas(16) uint8_t rbuf[0x20] = {};
    const float* rot = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(ROTATION))(main, rbuf);
    if (!phys || !rot) return SetMessage("Couldn't tell where you are");

    // they arrive a little way back along the host's path (or right here if there's no such spot), the summons a
    // couple of metres around the invader (a spirit ash's own characters spread out from there as it does); those
    // coming back where they stood on the other game
    alignas(16) float origin[4] = { phys[0], phys[1], phys[2], phys[3] };
    float away = ArrivalPoint(main, now, origin);
    // an automatic invasion with no such spot yet (none of the host's path in this block is 15 m from everyone) waits for
    // one rather than coming in on top of the party (2026-10-30: four at 0-1 m, and the host died); a menu summon comes
    if (away <= 0 && automatic) {
        if (p.tries % 9 == 0)
            Log("CoopResync: invasions: an automatic invasion has no ground to arrive on here yet; it waits (%llu s so far)",
                (unsigned long long)(p.tries * kArriveRetryMs / 1000));
        p.tries++;
        p.at = now + kArriveRetryMs;
        s_Pending = p;
        return;
    }
    static const float kAround[4][2] = { { 2.0f, 0.0f }, { -2.0f, 0.0f }, { 0.0f, 2.0f }, { 0.0f, -2.0f } };
    int company = (int)p.members.size() - 1, upgrade = AshUpgrade(level);
    std::string handles;
    for (size_t mi = 0; mi < p.members.size(); mi++) {
        const PendingMember& m = p.members[mi];
        alignas(16) float at[4] = { origin[0], origin[1], origin[2], origin[3] };
        bool there = p.back && PhysicsAt(main, m.spot, at);
        if (there) {
            float dx = at[0] - phys[0], dy = at[1] - phys[1], dz = at[2] - phys[2];
            if (dx * dx + dy * dy + dz * dz > 150.0f * 150.0f) {   // too far to be loaded here: it comes in with the others
                memcpy(at, origin, sizeof(at));
                there = false;
            }
        }
        if (mi && !there) {
            at[0] += kAround[(mi - 1) % 4][0];
            at[2] += kAround[(mi - 1) % 4][1];
        }
        std::vector<uint64_t> before;
        for (uintptr_t c : InvaderChrs()) before.push_back(Rd<uint64_t>(c + 8));
        // the summon manager summons for the main player: give it the ghost's number for this one call
        int32_t number = Rd<int32_t>(pgd + 8), req = Rd<int32_t>(man + 0x20), active = Rd<int32_t>(man + 0x24);
        memcpy(reinterpret_cast<void*>(man + 0xa0), at, sizeof(at));
        *reinterpret_cast<volatile float*>(man + 0xb0) = rot[1];
        *reinterpret_cast<volatile int32_t*>(man + 0x20) = m.pick ? m.pick->buddy : m.ash->trigger + upgrade;
        *reinterpret_cast<volatile int32_t*>(pgd + 8) = kGhostOwner;
        bool ok = CallSummon(man);
        *reinterpret_cast<volatile int32_t*>(pgd + 8) = number;
        *reinterpret_cast<volatile int32_t*>(man + 0x20) = req;
        *reinterpret_cast<volatile int32_t*>(man + 0x24) = active;
        s_LastInvaderAlive = now;
        if (!ok) {
            Log("CoopResync: invasions: the summon of %s failed", m.name.c_str());
            if (!mi) return SetMessage("The summon failed (see the log)");
            continue;
        }
        int made = 0;
        for (uintptr_t c : InvaderChrs()) {
            uint64_t h = Rd<uint64_t>(c + 8);
            bool old = false;
            for (uint64_t b : before) old |= b == h;
            if (old || ActiveByHandle(h)) continue;
            *reinterpret_cast<volatile uint8_t*>(c + 0x6c) = kInvaderTeam;
            ActiveInvader a = { h, m.name, m.pick ? m.pick->title : std::string(), now, m.pick ? 3 : 0, now };
            a.own = true;
            a.named = true;
            a.bucket = m.pick ? m.bucket : 0;
            a.ash = !m.pick;
            a.summon = mi > 0;
            if (!mi) a.company = company;
            a.build = m.pick;
            a.ashEntry = m.ash;
            memcpy(a.armor, m.armor, sizeof(a.armor));
            a.dressed = m.dressed;
            if (p.back) {
                // as it was (its HP and defense rows stay the ones decided when it first came)
                a.title = m.title;
                a.hpRow = m.hpRow;
                a.defRow = m.defRow;
                a.hpDecided = true;
                a.resumeHp = m.hp;
                a.summon = m.summon;
                a.company = m.company;
                a.back = true;
            }
            s_Active.push_back(a);
            made++;
            char hb[24];
            snprintf(hb, sizeof(hb), "%s%llX", handles.empty() ? "" : ", ", (unsigned long long)h);
            handles += hb;
        }
        std::string where = there ? "where it stood on their game"
                          : away > 0 ? std::to_string((int)away) + " m away" : std::string("right here");
        if (m.pick)
            Log("CoopResync: invasions: %s %s (%s, buddy %d, invader level %d, host level %d, catch-up step %d, %s): %d character(s)",
                p.back ? "back:" : mi ? "with it," : !automatic ? "summoned" : p.tries ? "automatic (it waited for ground)" : "automatic",
                m.name.c_str(), m.pick->title.c_str(), m.pick->buddy, m.pick->level, level, m.bucket, where.c_str(), made);
        else
            Log("CoopResync: invasions: %s spirit ash %s +%d (summon %d, %s): %d character(s)", p.back ? "back:" : "with it,",
                m.name.c_str(), upgrade, m.ash->trigger + upgrade, where.c_str(), made);
        if (!mi && !made) return SetMessage("The summon made no invader (see the log)");
    }
    // where they came in, against everyone else here (physics positions share a frame within a block)
    std::string others;
    for (const Copy& c : PlayerSet()) {
        if (c.chr == main) continue;
        float q[3];
        char buf[80];
        if (Rd<uint32_t>(c.chr + 0x30) == Rd<uint32_t>(main + 0x30) && PhysicsPos(c.chr, q)) {
            float dx = q[0] - origin[0], dy = q[1] - origin[1], dz = q[2] - origin[2];
            snprintf(buf, sizeof(buf), "%s%llX %.0f m", others.empty() ? "" : ", ", (unsigned long long)SteamIdOf(c.chr),
                     sqrtf(dx * dx + dy * dy + dz * dz));
        } else {
            snprintf(buf, sizeof(buf), "%s%llX in another block", others.empty() ? "" : ", ", (unsigned long long)SteamIdOf(c.chr));
        }
        others += buf;
    }
    Log("CoopResync: invasions/diag: summoned handle(s) %s at (%.1f, %.1f, %.1f) block %08X; other players from there: %s; groups: %s",
        handles.empty() ? "none" : handles.c_str(), origin[0], origin[1], origin[2], Rd<uint32_t>(main + 0x30),
        others.empty() ? "none" : others.c_str(), GroupsText(BuddyGroups()).c_str());
    s_LastInvasion = now;
    const PendingMember& lead = p.members[0];
    if (p.back) {
        std::string line = lead.name + (company ? " and " + std::to_string(company) + " more are back!" : " is back!");
        ShowNotice(line, "");
        return SetMessage(line);
    }
    ShowBanner(lead.name, CompanyTitle(lead.pick->title, company));
    SetMessage("Invaded by " + lead.name + (company ? " and " + std::to_string(company) + (company == 1 ? " summon!" : " summons!") : "!"));
}

struct PhantomSpec {
    uint32_t unk0;
    int32_t phantomParam;
    float unk8;
    uint8_t unkc[4];
};

static bool CallPhantomApply(uintptr_t modifiers, const PhantomSpec* spec) {
    __try {
        reinterpret_cast<void (*)(uintptr_t, const PhantomSpec*, uint8_t)>(Addr(PHANTOM_APPLY))(modifiers, spec, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void PaintInvader(uintptr_t c, ActiveInvader& a, int phantom) {
    if (Rd<int32_t>(c + 0x540) != phantom) *reinterpret_cast<volatile int32_t*>(c + 0x540) = phantom;
    bool painted = false;
    if (!a.own && Intact(PHANTOM_APPLY, true)) {
        uintptr_t mods = Ptr(c + 0x190);
        uintptr_t modifiers = mods ? Ptr(mods + 0xd0) : 0;
        PhantomSpec spec = { 0, phantom, 0.0f, {} };
        painted = modifiers && CallPhantomApply(modifiers, &spec);
    }
    a.painted = painted;
    if (!a.looked) {
        a.looked = true;
        Log("CoopResync: invasions: %s in PhantomParam %d (%s; npc %d, team %d)", a.name.c_str(), phantom,
            a.own ? "ours: phantom override" : painted ? "another game's: painted" : "another game's: NOT painted",
            Rd<int32_t>(c + 0x60), Rd<uint8_t>(c + 0x6c));
    }
}

// Another game: our copies of invaders carrying on alone, all kFleeMetres off for 5 s (or out of reach): we got away
static ULONGLONG s_FarSince = 0, s_GotAwayAt = 0;
static void GetAway(uintptr_t main, ULONGLONG now) {
    std::vector<const ActiveInvader*> alone;
    bool close = false;
    Spot me;
    bool placed = main && Hp(main) > 0 && SafeSpotOf(main, me);
    for (const ActiveInvader& a : s_Active) {
        if (a.own || !a.orphan || !a.chr || a.lastHp <= 0) continue;
        alone.push_back(&a);
        Spot s;
        float d = placed && SafeSpotOf(a.chr, s) ? SpotMetres(me, s) : 0.0f;  // -1: another space
        close |= d >= 0.0f && d < kFleeMetres;
    }
    if (alone.empty() || close) {
        s_FarSince = 0;
        return;
    }
    if (!s_FarSince) s_FarSince = now;
    if (now - s_FarSince < 5000) return;
    s_FarSince = 0;
    for (const ActiveInvader* a : alone) DropCopy(*a, now);
    if (now - s_GotAwayAt < 30000) return;
    s_GotAwayAt = now;
    Log("CoopResync: invasions: got away from %zu invader(s) carrying on alone here (all over %d m off for 5 s): dropping them",
        alone.size(), (int)kFleeMetres);
    ShowNotice("You got away!", "");
}

// Go to is refused with a live invader this close (ours or another game's copy), as the game's fast travel is with
// enemies about: no warping away from a fight
static constexpr float kOnYouMetres = 50.0f;
static bool InvaderOnYou(uintptr_t main) {
    Spot me;
    if (!main || !SafeSpotOf(main, me)) return false;
    for (const ActiveInvader& a : s_Active) {
        Spot s;
        if (!a.chr || a.lastHp <= 0 || !SafeSpotOf(a.chr, s)) continue;
        float d = SpotMetres(me, s);
        if (d >= 0.0f && d < kOnYouMetres) return true;
    }
    return false;
}

// The summoner: forget invaders of ours whose copy elsewhere is gone too; once we're back within kReturnMetres of
// those carrying on alone (alive, free, nothing else on the way), that game drops its copies (we wait for its word,
// or for its live reports to stop), then they're summoned here where they stand, with the HP they have left
static ULONGLONG s_ReturnAt = 0, s_ReturnRetryAt = 0, s_NextDrop = 0;
static bool s_HuntedShown = false;
static void ReturnFrame(uintptr_t main, ULONGLONG now) {
    for (size_t i = 0; i < s_GoneOwn.size();) {
        GoneInvader& g = s_GoneOwn[i];
        bool quiet = g.heardAt && now - g.heardAt > kOrphanQuietMs;
        if (!g.asked && g.heardAt && (g.dropped || g.hp <= 0 || quiet)) {
            Log("CoopResync: invasions: our invader %s no longer carries on on %s's game (%s)", g.was.name.c_str(),
                g.holderName.c_str(), g.hp <= 0 ? "defeated there" : g.dropped ? "no copy there now" : "no word from there");
            s_GoneOwn.erase(s_GoneOwn.begin() + i);
        } else if (!g.heardAt && now - g.at > 60000) {
            s_GoneOwn.erase(s_GoneOwn.begin() + i);
        } else {
            i++;
        }
    }
    std::vector<GoneInvader*> out;
    for (GoneInvader& g : s_GoneOwn)
        if (g.heardAt && g.hp > 0) out.push_back(&g);
    if (out.empty()) {
        for (GoneInvader& g : s_GoneOwn) g.asked = false;  // all defeated there while asked for: forgotten next frame
        s_ReturnAt = 0;
        s_HuntedShown = false;
        return;
    }
    if (!s_ReturnAt) {
        if (now < s_ReturnRetryAt || s_Pending.on || !main || InvasionBlocked(main)) return;
        if (!s_HuntedShown) {   // once we're up again (not on the loading screen)
            s_HuntedShown = true;
            ShowNotice(out[0]->holderName + " is still hunted!", "Get back there: your invaders return when you're near");
        }
        Spot me;
        if (!SafeSpotOf(main, me)) return;
        float nearest = -1.0f;
        for (GoneInvader* g : out) {
            float d = g->spot.block != -1 ? SpotMetres(me, g->spot) : -1.0f;
            if (d >= 0.0f && (nearest < 0.0f || d < nearest)) nearest = d;
        }
        if (nearest < 0.0f || nearest > kReturnMetres) return;
        s_ReturnAt = now;
        s_NextDrop = 0;
        Log("CoopResync: invasions: back within %d m of %zu invader(s) of ours carrying on alone on %s's game: asking for them back",
            (int)nearest, out.size(), out[0]->holderName.c_str());
    }
    // dropped there: its last report says so, or none of its live reports for 4.5 s since we asked
    bool all = true;
    for (GoneInvader* g : out) {
        g->asked = true;
        all &= g->dropped || (now - s_ReturnAt > 4500 && now - g->heardAt > 4500);
    }
    if (now >= s_NextDrop) {
        s_NextDrop = now + 1000;
        std::vector<Member> members = Members();
        for (GoneInvader* g : out) {
            if (g->dropped) continue;
            for (const Member& m : members)
                if (!m.isLocal && m.steamId) SendGone(m.steamId, *g, kGoneDrop, now);   // every game with a copy
        }
    }
    if (!all) {
        if (now - s_ReturnAt < 12000) return;
        Log("CoopResync: invasions: %s's game didn't drop our invaders within 12 s; they carry on there (asking again in 30 s)",
            out[0]->holderName.c_str());
        for (GoneInvader& g : s_GoneOwn) g.asked = false;
        s_ReturnAt = 0;
        s_ReturnRetryAt = now + 30000;
        return;
    }
    PendingInvasion p;
    p.on = true;
    p.back = true;
    p.at = now;
    std::string holder = out[0]->holderName;
    for (GoneInvader* g : out) {
        const ActiveInvader& w = g->was;
        if (!w.build && !w.ashEntry) continue;
        PendingMember m;
        m.pick = w.build;
        m.ash = w.ashEntry;
        m.name = w.name;
        m.bucket = w.bucket;
        memcpy(m.armor, w.armor, sizeof(m.armor));
        m.dressed = w.dressed;
        m.spot = g->spot;
        m.hp = g->hp;
        m.title = w.title;
        m.hpRow = w.hpRow;
        m.defRow = w.defRow;
        m.summon = w.summon;
        m.company = w.company;
        p.members.push_back(m);
    }
    // all that were asked for go from the list (those defeated there meanwhile too)
    for (size_t i = 0; i < s_GoneOwn.size();)
        if (s_GoneOwn[i].asked) s_GoneOwn.erase(s_GoneOwn.begin() + i);
        else i++;
    s_ReturnAt = 0;
    if (p.members.empty()) return;
    std::stable_partition(p.members.begin(), p.members.end(), [](const PendingMember& m) { return !m.summon; });
    bool others = InSession(LobbyState()) && Members().size() > 1;
    for (const PendingMember& m : p.members) {
        if (!m.pick || !m.dressed) continue;
        DressRow(m.pick->chara, m.armor);
        if (others) SendArmor(m.pick->chara, m.armor);
    }
    if (others) p.at = now + kArmorLeadMs;
    Log("CoopResync: invasions: %s's game dropped its copies; %zu invader(s) of ours come back here where they stand",
        holder.c_str(), p.members.size());
    if (s_Pending.on) Log("CoopResync: invasions: (the invasion that was on its way makes way for them)");
    s_Pending = p;
}

// Every game, every frame: invaders hostile, scaled to the party, in the invader look and named; the
// summoner's game keeps the ghost owner present while one lives; announcements go out a few times
// (messages are unreliable); each game rolls its player's loot when an invader dies; the host's game
// runs the automatic invasions; invaders gone for a while are forgotten
static void InvasionFrame(ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    int party = InSession(lobby) ? (int)Members().size() : 1;
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    int level = pgd ? Rd<int32_t>(pgd + 0x68) : 1;
    TrackGroups(BuddyGroups(), now);
    if (lobby != 6) ScanArea(main, now);
    if (!s_NameHookTried && main) HookNames();
    if (uint32_t c = s_NewNameCategory.exchange(0)) Log("CoopResync: invasions: invader names answered in text category %u", c - 1);
    RecordTrail(main, now);
    std::vector<InvaderView> view;
    bool alive = false;
    for (uintptr_t c : InvaderChrs()) {
        if (Rd<uint8_t>(c + 0x6c) != kInvaderTeam) *reinterpret_cast<volatile uint8_t*>(c + 0x6c) = kInvaderTeam;
        uint64_t h = Rd<uint64_t>(c + 8);
        ActiveInvader* a = ActiveByHandle(h);
        if (!a) {
            s_Active.push_back({ h, "Tarnished", "", now, 0, 0 });
            a = &s_Active.back();
            if (const AshEntry* ash = AshByNpc(Rd<int32_t>(c + 0x60))) {
                a->name = ash->name;
                a->named = true;
                a->ash = true;
                a->bucket = 0;
            }
        }
        a->seen = now;
        if (a->chr != c) {
            DiagAppeared(*a, c, main, now);
            for (size_t i = 0; i < s_GoneOwn.size(); i++)
                if (s_GoneOwn[i].handle == h) {
                    s_GoneOwn.erase(s_GoneOwn.begin() + i);
                    break;
                }
        }
        a->chr = c;
        a->lastHp = Hp(c);
        if (now >= a->nextStatus && a->statusLines < 40) {
            a->nextStatus = now + kStatusEveryMs;
            a->statusLines++;
            Log("CoopResync: invasions/diag: %llX (%s) here: %s%s", (unsigned long long)h, a->name.c_str(), CopyText(c, main).c_str(),
                a->own ? "" : a->painted ? ", painted" : ", NOT painted");
        }
        if (a->named && !a->nameShown) {
            SetInvaderName(Rd<int32_t>(c + 0x60), a->name);
            a->nameShown = true;
        }
        int hp = Hp(c);
        uintptr_t mods = Ptr(c + 0x190);
        uintptr_t data = mods ? Ptr(mods) : 0;
        int maxHp = data ? Rd<int32_t>(data + 0x13c) : 0;
        if (maxHp > 0) a->maxHp = maxHp;
        if (hp > 0) {
            alive = true;
            s_LastInvaderAlive = now;
            if (s_Inv.look > 0) PaintInvader(c, *a, s_Inv.look);
            // toughness = party size x level catch-up, and HP and defense like the game's NPC invaders in the host's
            // area (the summoner's game decides those on its own copy's first frame, before anything of ours is on
            // it, and says); another game's invader waits up to 3 s for word. Spirit ashes get the party toughness
            if (!a->since) a->since = now;
            if (a->own && !a->ash && !a->hpDecided && (maxHp > 0 || now - a->since > 3000)) DecideHp(*a, c, level);
            bool ready = a->ash || (a->own ? a->hpDecided : a->bucket >= 0 || now - a->since > 3000);
            int bucket = a->bucket >= 0 ? a->bucket : 0;
            int32_t want[3] = { ready && s_ScaleBase ? s_ScaleBase + 1000 * (std::min)(party, 6) + (std::min)(bucket, s_ScaleLast) : 0,
                                ready ? a->hpRow : 0, ready ? a->defRow : 0 };
            if (memcmp(want, a->applied, sizeof(want)) != 0 && Intact(APPLY_SPEFFECT, true) && Intact(REMOVE_SPEFFECT, true)) {
                for (int i = 0; i < 3; i++) {
                    if (want[i] == a->applied[i]) continue;
                    if (a->applied[i]) reinterpret_cast<void (*)(uintptr_t, int32_t)>(Addr(REMOVE_SPEFFECT))(c, a->applied[i]);
                    if (want[i]) reinterpret_cast<uint64_t (*)(uintptr_t, int32_t, uint8_t)>(Addr(APPLY_SPEFFECT))(c, want[i], 1);
                    a->applied[i] = want[i];
                }
                Log("CoopResync: invasions: %s scaled for %d player(s), level step %d, HP %d, defense %d (SpEffects %d, %d, %d)",
                    a->name.c_str(), party, bucket, a->hpRow, a->defRow, want[0], want[1], want[2]);
                if (a->own) a->topUpAt = now + 500;
            }
            if (a->topUpAt && now >= a->topUpAt && data) {
                *reinterpret_cast<volatile int32_t*>(data + 0x138) = a->resumeHp > 0 ? (std::min)(a->resumeHp, maxHp) : maxHp;
                if (a->resumeHp > 0)
                    Log("CoopResync: invasions: %s back with %d of %d HP", a->name.c_str(), (std::min)(a->resumeHp, maxHp), maxHp);
                a->resumeHp = 0;
                a->topUpAt = 0;
            }
        } else if (!a->rewarded && !a->ash) {
            a->rewarded = true;
            if (!s_Loot.empty() && (int)Random(100) < s_Inv.lootChance) {
                const LootItem& item = s_Loot[Random((uint32_t)s_Loot.size())];
                bool given = ConvenienceMenu::GiveItem(item.id, item.count);
                Log("CoopResync: invasions: %s defeated; loot %s x%d (%08X) %s", a->name.c_str(), item.name.c_str(), item.count,
                    item.id, given ? "given" : "NOT given");
                if (given) ShowReward(item.count > 1 ? item.name + " x" + std::to_string(item.count) : item.name);
            } else {
                // Consolation: Golden Rune [1]-[13] (goods 2900-2912), each step up 70% as likely as the last
                int n = 1;
                while (n < 13 && Random(100) < 70) n++;
                std::string rune = "Golden Rune [" + std::to_string(n) + "]";
                bool given = ConvenienceMenu::GiveItem(0x40000000u + 2899 + n, 1);
                Log("CoopResync: invasions: %s defeated; no loot roll, consolation %s %s", a->name.c_str(), rune.c_str(),
                    given ? "given" : "NOT given");
                if (given) ShowReward(rune);
            }
        }
        view.push_back({ a->name, a->title, hp, maxHp });
    }
    // diagnostics: the summon groups when they change; copies that left; announced invaders that never
    // came; reports of another game's invader to the others
    std::vector<GroupEntry> groups = BuddyGroups();
    std::string sig;
    for (const GroupEntry& g : groups) {
        char b[64];
        snprintf(b, sizeof(b), "%d:%llX:%d:%d%d;", g.owner, (unsigned long long)g.chr, g.buddy, g.leaving, g.remote);
        sig += b;
    }
    if (sig != s_GroupsSeen) {
        s_GroupsSeen = sig;
        if (s_GroupLogs++ < 300) Log("CoopResync: invasions/diag: summon groups here now: %s", GroupsText(groups).c_str());
    }
    for (ActiveInvader& a : s_Active) {
        if (a.chr && a.seen != now) {
            Log("CoopResync: invasions/diag: copy of %llX (%s) left the buddy set here after %llu s (last hp %d)%s; groups: %s",
                (unsigned long long)a.handle, a.name.c_str(), (unsigned long long)((now - a.appeared) / 1000), a.lastHp,
                ChrByHandle(a.handle) ? ", its handle still finds a character" : "", GroupsText(groups).c_str());
            a.chr = 0;
            if (a.own && a.lastHp > 0) {
                // alive when it went (we died or reloaded): another game's copy may carry on alone
                if (s_GoneOwn.size() >= 64) s_GoneOwn.erase(s_GoneOwn.begin());
                s_GoneOwn.push_back({ a.handle, now, main && Hp(main) <= 0, a });
                if (s_GoingTo && now - s_GoingAt < 60000) s_GoneOwn.back().comingTo = s_GoingTo;
            } else if (!a.own && a.orphan) {
                SendReport(a, main, now);  // its last report: no copy here (the summoner may be waiting for it)
            }
        }
        if (!a.own && a.newsAt && !a.appeared && !a.missingLogged && now - a.newsAt > 3000) {
            a.missingLogged = true;
            std::string here;
            for (uintptr_t c : InvaderChrs()) {
                char b[64];
                snprintf(b, sizeof(b), "%s%llX (npc %d)", here.empty() ? "" : ", ", (unsigned long long)Rd<uint64_t>(c + 8), Rd<int32_t>(c + 0x60));
                here += b;
            }
            Log("CoopResync: invasions/diag: %llX (%s) was announced %llu ms ago but no copy of it has been in the buddy set here; its "
                "handle %s; invaders here: %s; groups: %s; invader groups dismissed here so far: %u", (unsigned long long)a.handle,
                a.name.c_str(), (unsigned long long)(now - a.newsAt), ChrByHandle(a.handle) ? "finds a character" : "finds nothing",
                here.empty() ? "none" : here.c_str(), GroupsText(groups).c_str(), s_GhostDismissals.load());
        }
        // (past the first 60, only while a live copy is here: the summoner answers an orphan's report)
        bool reporting = a.reports < 60 || (a.chr && a.lastHp > 0);
        if (!a.own && (a.chr || (a.newsAt && now - a.newsAt < 60000)) && now >= a.nextReport && reporting) {
            a.nextReport = now + kReportEveryMs;
            a.reports++;
            SendReport(a, main, now);
        }
    }
    for (ActiveInvader& a : s_Active) {
        if (a.sends > 0 && now >= a.nextSend && (!a.own || a.hpDecided)) {
            SendInvader(a);
            a.sends--;
            a.nextSend = now + 1000;
        }
    }
    for (size_t i = 0; i < s_Active.size();)
        if (now - s_Active[i].seen > 30000) s_Active.erase(s_Active.begin() + i);
        else i++;
    GetAway(main, now);
    if (lobby != 6) ReturnFrame(main, now);
    for (const GoneInvader& g : s_GoneOwn)
        if (g.heardAt && g.hp > 0 && !g.dropped && now - g.heardAt < kOrphanQuietMs)
            view.push_back({ g.was.name, "hunting " + g.holderName, g.hp, (std::max)(g.was.maxHp, g.hp) });

    // automatic invasions: the host (or a solo player) rolls every checkSeconds; the chance climbs
    // from 0 to certain over guaranteeMinutes since the last invasion
    if (s_Inv.autoOn && lobby != 6 && main) {
        if (!s_LastInvasion) s_LastInvasion = now;
        float minutes = (now - s_LastInvasion) / 60000.0f;
        s_ChanceNow = fminf(1.0f, powf(minutes / fmaxf(1.0f, s_Inv.guaranteeMinutes), s_Inv.rampPower));
        if (now >= s_NextCheck) {
            s_NextCheck = now + (ULONGLONG)fmaxf(5.0f, (float)s_Inv.checkSeconds) * 1000;
            if (!alive && !s_Pending.on && !OrphansOut(now) && Random(1000000) < (uint32_t)(s_ChanceNow * 1000000.0f)) {
                const char* why = InvasionBlocked(main);
                if (why) Log("CoopResync: invasions: an automatic invasion is due but waits (%s)", why);
                else SummonInvader(now, true);
            }
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_InvaderView = std::move(view);
}

// ---- party scaling ----

// Seamless Co-op scales an enemy for the party size when the enemy is set up (it replaces the game's "apply
// multiplay correction", which every game runs on its own copies), but leaves the enemies already loaded when
// someone joins or leaves as they were. So the host's enemies stayed at the old size while a guest's fresh copies
// got the new one, and the guest saw the host's HP against a bigger maximum (1k / 2k), with the enemies really
// as tough as before. When the party size changes, every game hands its loaded enemies to that function again, and
// the host, whose copies hold the real HP, keeps each one's share of its HP. Only the ones scaled when they're set up
// (ordinary and elite enemies, correction mode 1): under Seamless no game scales field bosses (mode 2: the set-up's
// own check counts no co-op players), and main bosses (mode 0) are scaled when their fight starts.
// (chr, &lookup {+0 row id, +8 MultiPlayCorrectionParam row}): apply the scaling SpEffect for the party size
SITE(MPC_APPLY, "apply multiplay correction", 0x3faf60, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57);
// (chr, &lookup): the chr's MultiPlayCorrectionParam row (NpcParam +0x1d8), row null when it has none
SITE(MPC_LOOKUP, "multiplay correction lookup", 0x3f0690, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01, 0x48, 0x8b, 0xda, 0xff, 0x90, 0x78, 0x01);

struct MpcLookup {
    int32_t rowId;
    uint32_t pad;
    const uint8_t* row;
    uint64_t spare[2];
};

static int s_ScaleParty = 1;   // the party size the loaded enemies were last handed over for
static ULONGLONG s_ScaleAt = 0;
static int s_ScalePasses = 0;  // passes still to run for this change (Seamless's own count may follow a little late)
static int s_ScaleOk = -1;     // -1 not checked yet

static bool ScaleOk() {
    if (s_ScaleOk < 0) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(Addr(MPC_APPLY));
        // only with Seamless's replacement in front: the game's own would count no co-op players and end the scaling
        bool hooked = Intact(MPC_APPLY, true) && memcmp(p, MPC_APPLY_BYTES, sizeof(MPC_APPLY_BYTES)) != 0;
        s_ScaleOk = hooked && Intact(MPC_LOOKUP, false);
        Log("CoopResync: party scaling: %s", s_ScaleOk ? "on (Seamless Co-op's scaling is in place)"
                                                      : "off (the game's scaling isn't Seamless Co-op's here)");
    }
    return s_ScaleOk == 1;
}

static void RescaleLoaded(int party, bool host) {
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    if (!wcm) return;
    std::vector<uintptr_t> skip;  // players and the summon-buddy set (spirit ashes, Torrent, invaders)
    for (const Copy& c : PlayerSet()) skip.push_back(c.chr);
    uint32_t cap = Rd<uint32_t>(wcm + 0x10f90 + 0x10);
    uintptr_t buddies = Ptr(wcm + 0x10f90 + 0x18);
    for (uint32_t i = 0; buddies && i < cap && i < 1000; i++)
        if (uintptr_t c = Ptr(buddies + i * 16)) skip.push_back(c);
    std::sort(skip.begin(), skip.end());
    auto lookup = reinterpret_cast<void (*)(uintptr_t, MpcLookup*)>(Addr(MPC_LOOKUP));
    auto apply = reinterpret_cast<void (*)(uintptr_t, MpcLookup*)>(Addr(MPC_APPLY));
    int handed = 0, changed = 0, visited = 0, exampleFrom = 0, exampleTo = 0;
    for (int set = 0; set < 196 && visited < 8000; set++) {
        uintptr_t cs = Ptr(wcm + WCM_CHR_SETS + set * 8);
        uintptr_t entries = cs ? Ptr(cs + 0x18) : 0;
        uint32_t n = cs ? Rd<uint32_t>(cs + 0x10) : 0;
        for (uint32_t i = 0; entries && i < n && i < 2048 && visited < 8000; i++, visited++) {
            uintptr_t c = Ptr(entries + i * 16);
            if (!c || Hp(c) <= 0 || IsInvader(c) || std::binary_search(skip.begin(), skip.end(), c)) continue;
            MpcLookup lk{};
            lookup(c, &lk);
            if (!Valid(reinterpret_cast<uintptr_t>(lk.row))) continue;
            if (lk.row[0x10] != 1) continue;
            int hp = Hp(c), oldMax = MaxHp(c);
            apply(c, &lk);
            handed++;
            int newMax = MaxHp(c);
            if (newMax <= 0 || oldMax <= 0 || newMax == oldMax) continue;
            changed++;
            if (!exampleFrom) exampleFrom = oldMax, exampleTo = newMax;
            uintptr_t mods = Ptr(c + 0x190);
            uintptr_t data = mods ? Ptr(mods) : 0;
            if (host && data)
                *reinterpret_cast<volatile int32_t*>(data + 0x138) =
                    (std::max)(1, (std::min)(newMax, (int)llround((double)hp * newMax / oldMax)));
        }
    }
    Log("CoopResync: party scaling: %d player(s): %d enemies scaled again, %d with a new maximum (e.g. %d -> %d)%s",
        party, handed, changed, exampleFrom, exampleTo, host ? ", their HP share kept" : "");
}

static void PartyScalingFrame(ULONGLONG now) {
    int lobby = LobbyState();
    std::vector<Member> members = InSession(lobby) ? Members() : std::vector<Member>();
    int party = members.empty() ? 1 : (int)members.size();
    if (party != s_ScaleParty) {
        s_ScaleParty = party;
        s_ScaleAt = now + 3000;
        s_ScalePasses = 2;
    }
    if (!s_ScalePasses || now < s_ScaleAt || !ScaleOk()) return;
    uintptr_t gm = Global(GAME_MAN);
    if (!MainPlayer() || (gm && Rd<uint8_t>(gm + 0x10))) {  // loading: once it's done
        s_ScaleAt = now + 1000;
        return;
    }
    bool host = members.empty();
    for (const Member& m : members) host |= m.isLocal && m.isHost;
    RescaleLoaded(party, host);
    s_ScalePasses--;
    s_ScaleAt = now + 10000;
}

// ---- the per-frame task ----

static ULONGLONG s_NextSnapshot = 0;

static void RunFrame() {
    ULONGLONG now = GetTickCount64();
    if (LinkOk() && InSession(LobbyState())) {
        ReceiveSpots(now);
        SendSpot(now);
    }
    if (s_WantResync.exchange(false)) StartResync(now);
    if (s_WantReload.exchange(false)) ReloadHere();
    if (uint64_t to = s_WantGoTo.exchange(0)) GoTo(to, now);
    if (s_WantClearBoss.exchange(false)) ClearBossFlag();
    WatchReload(now);
    if (s_InvasionsOk) {
        if (s_WantInvader.exchange(false)) SummonInvader(now, false);
        if (s_Pending.on && now >= s_Pending.at) SummonPending(now);
        InvasionFrame(now);
    }
    if (s_FollowUpAt && now >= s_FollowUpAt) {
        s_FollowUpAt = 0;
        FollowUp(now);
    }
    AutoResync(now);
    if (now >= s_NextSnapshot) {
        s_NextSnapshot = now + 250;
        Snapshot();
    }
}

// "Find an item" (drop_finder.h) runs on the same task with a guard of its own
static bool s_FinderFaulted = false;
static void FinderFrame() {
    if (s_FinderFaulted) return;
    __try {
        DropFinder::RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_FinderFaulted = true;
        Log("DropFinder: exception in the per-frame task (code %08X); Find an item turned off", GetExceptionCode());
    }
}

static bool s_MinimapFaulted = false;
static void MinimapFrame() {
    if (s_MinimapFaulted) return;
    __try {
        Minimap::RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_MinimapFaulted = true;
        Log("Minimap: exception in the per-frame task (code %08X); the minimap turned off", GetExceptionCode());
    }
}

static bool s_TargetFaulted = false;
static void TargetFrame() {
    if (s_TargetFaulted) return;
    __try {
        if (World::Ready()) HudIcons::Install();
        Target::RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_TargetFaulted = true;
        Log("Target: exception in the per-frame task (code %08X); the target panel turned off", GetExceptionCode());
    }
}

static bool s_HorseUpFaulted = false;
static void HorseUpFrame() {
    if (s_HorseUpFaulted) return;
    __try {
        HorseUp::RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_HorseUpFaulted = true;
        Log("HorseUp: exception in the per-frame task (code %08X); Horse Up turned off", GetExceptionCode());
    }
}

static bool s_FlyingMountFaulted = false;
static void FlyingMountFrame() {
    if (s_FlyingMountFaulted) return;
    __try {
        FlyingMount::RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_FlyingMountFaulted = true;
        Log("FlyingMount: exception in the per-frame task (code %08X); flying turned off", GetExceptionCode());
    }
}

static bool s_ScaleFaulted = false;
static void ScalingFrame() {
    if (s_ScaleFaulted) return;
    __try {
        PartyScalingFrame(GetTickCount64());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_ScaleFaulted = true;
        Log("CoopResync: party scaling: exception in the per-frame task (code %08X); turned off", GetExceptionCode());
    }
}

static void TaskExecute(void*, const void*) {
    FinderFrame();
    MinimapFrame();
    TargetFrame();
    HorseUpFrame();
    FlyingMountFrame();
    ScalingFrame();
    if (s_Faulted) return;
    s_TaskSeen = true;
    __try {
        RunFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_RebuildThread = 0;
        s_Faulted = true;
        Log("CoopResync: exception in the per-frame task (code %08X); co-op resync turned off", GetExceptionCode());
    }
}

// Looks like one of the game's own tasks to its task runner, which only calls Execute (slot 2)
struct TaskVtbl {
    void* (*runtimeClass)(void*);
    void (*destructor)(void*);
    void (*execute)(void*, const void*);
};
static void* TaskRuntimeClass(void*) { return nullptr; }
static void TaskDestructor(void*) {}
static const TaskVtbl s_TaskVtbl = { TaskRuntimeClass, TaskDestructor, TaskExecute };
struct alignas(16) TaskObject {
    const TaskVtbl* vtbl;
    uintptr_t unk8;
    uint8_t spare[0x30];
};
static TaskObject s_Task = { &s_TaskVtbl, 0, {} };

// ---- public ----

void Init() {
    s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!s_Base) return;
    LoadCoopSettings();
    s_NetIdOk = Intact(NET_ID_GETTER, false) && Rd<uintptr_t>(s_Base + NET_SESSION_VTABLE + 0x48) == Addr(NET_ID_GETTER);
    s_ReviveOk = Intact(VF180, true);
    s_RebuildOk = Intact(REMOVE_SLOT, true) && Intact(REMOVE_CHR, true) && Intact(BEFORE_REMOVE, true) && Intact(PMI_GETTER, false);
    s_ReloadOk = Intact(MAP_RELOAD, true) && Intact(SET_RELOAD_BLOCK, true) && Intact(SET_RELOAD_POS, true);
    s_SpotOk = Intact(REF_BLOCK, true) && Intact(PHYS_POS, true) && Intact(TO_BLOCK_POS, true) && Intact(ROTATION, true) && Intact(NORM_BLOCK, true);
    s_HostSpotOk = Intact(HOST_SPOT_GETTER, false) && Intact(HOST_SPOT_READER, false);
    ResolveLink();
    LoadInvaders();
    s_InvasionsOk = !s_InvaderList.empty() && s_SpotOk && Intact(SUMMON_REQUEST, true) && ChainDismissCheck();
    if (!s_InvaderList.empty() && !s_InvasionsOk) Log("CoopResync: invasions: the summon or dismissal code is not as expected; off");
    if (Intact(MEMBER_LEFT, false)) {
        void* target = reinterpret_cast<void*>(Addr(MEMBER_LEFT));
        MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HkMemberLeft), reinterpret_cast<void**>(&s_OrigMemberLeft));
        if (st == MH_OK) st = MH_EnableHook(target);
        s_GuardHooked = st == MH_OK;
        Log("CoopResync: member-left guard hook: %d", st);
    } else {
        Log("CoopResync: member-left handler not as expected; the host's character won't be rebuilt on a guest");
    }
    Log("CoopResync: net id %d, revive %d, rebuild %d, reload %d, guard %d, spot %d, host spot %d, link %d", s_NetIdOk,
        s_ReviveOk, s_RebuildOk, s_ReloadOk, s_GuardHooked, s_SpotOk, s_HostSpotOk, LinkOk());
}

void StartTask() {
    if (!s_Base) return;
    if (!Intact(REGISTER_TASK, true)) {
        Log("CoopResync: task registration not as expected (other game version?); co-op resync off");
        s_TaskFailed = true;
        return;
    }
    uintptr_t tasks = 0;
    for (int i = 0; i < 240 && !(tasks = Global(CS_TASK)); i++) Sleep(500);
    if (!tasks) {
        Log("CoopResync: the game's task system never came up; co-op resync off");
        s_TaskFailed = true;
        return;
    }
    reinterpret_cast<void (*)(uintptr_t, uint32_t, void*)>(Addr(REGISTER_TASK))(tasks, 0, &s_Task);
    s_TaskRegistered = true;
    Log("CoopResync: per-frame task registered (CSTaskImp %llX)", (unsigned long long)tasks);
}

std::string Request(Action action) {
    if (!s_Base) return "Only for Elden Ring";
    if (s_Faulted) return "The co-op tools turned off after an error (see VinStyleRankGauge.log)";
    if (s_TaskFailed) return "Not available on this game version (see VinStyleRankGauge.log)";
    if (!(s_TaskRegistered && s_TaskSeen)) return "Not ready yet: load into the game first";
    std::vector<MemberView> view;
    int lobby;
    bool inGame;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        view = s_View;
        lobby = s_ViewLobby;
        inGame = s_ViewInGame;
    }
    switch (action) {
    case Action::Resync: {
        static std::atomic<ULONGLONG> last{ 0 };
        ULONGLONG now = GetTickCount64();
        if (!InSession(lobby)) return "Not in a co-op session";
        if (now - last.load() < 5000) return "Already resyncing, give it a few seconds";
        last = now;
        s_WantResync = true;
        return {};
    }
    case Action::Reload:
        s_WantReload = true;
        return {};
    case Action::GoToHost:
        if (lobby == 3) return "You're the host: F8 has Go to for each guest";
        if (lobby != 6) return "Not in a co-op session";
        for (const MemberView& m : view)
            if (m.isHost && m.steamId) {
                s_WantGoTo = m.steamId;
                return {};
            }
        return "Can't see the host yet";
    case Action::ClearBoss:
        s_WantClearBoss = true;
        return {};
    case Action::SummonInvader:
        if (!s_InvasionsOk) return "Invasions aren't available (see VinStyleRankGauge.log)";
        if (lobby == 6) return "The host summons invaders";
        if (!inGame) return "Load into the game first";
        s_WantInvader = true;
        return {};
    }
    return "Unknown request";
}

bool AutoResync() {
    LoadCoopSettings();
    return s_AutoResync;
}

void SetAutoResync(bool on) {
    LoadCoopSettings();
    s_AutoResync = on;
    SaveCoopSettings();
}

bool PositionCheck() {
    LoadCoopSettings();
    return s_PositionCheck;
}

void SetPositionCheck(bool on) {
    LoadCoopSettings();
    s_PositionCheck = on;
    SaveCoopSettings();
}

std::vector<Partner> Partners() {
    std::vector<MemberView> view;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        view = s_View;
    }
    std::vector<Partner> out;
    ULONGLONG now = GetTickCount64();
    for (const MemberView& m : view) {
        for (const Heard& h : s_Heard) {
            if (h.steamId != m.steamId || !h.liveAt || now - h.liveAt > kLiveFreshMs || h.live.block == -1 || !h.live.block) continue;
            out.push_back({ m.name, m.isHost, h.live.block, h.live.x, h.live.y, h.live.z, h.live.yaw, m.steamId });
        }
    }
    return out;
}

bool InHostsWorld() { return LobbyState() == 6; }

void SendHorseUp(const std::string& song) {
    void* net = LinkOk() && InSession(LobbyState()) ? s_NetMessages() : nullptr;
    if (!net) return;
    HorseMsg msg = { kHorseMagic, 1, 0, {} };
    strncpy(msg.song, song.c_str(), sizeof(msg.song) - 1);
    for (const Member& m : Members()) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        int result = s_Send(net, &to, &msg, sizeof(msg), 8 | 32 /* reliable, restart broken sessions */, kLinkChannel);
        Log("CoopResync: link: Horse Up %s sent to %llX (result %d)", msg.song, (unsigned long long)m.steamId, result);
    }
}

uint32_t LatestMessage(std::string& text) {
    std::lock_guard<std::mutex> lock(s_Lock);
    text = s_Message;
    return s_MessageSerial;
}

void DrawMenuSection(float scale) {
    std::vector<MemberView> view;
    std::vector<InvaderView> invaders;
    int lobby;
    bool inGame, boss;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        view = s_View;
        invaders = s_InvaderView;
        lobby = s_ViewLobby;
        inGame = s_ViewInGame;
        boss = s_ViewBoss;
        if (s_MessageAt && GetTickCount64() - s_MessageAt < 10000) message = s_Message;
    }

    const ImVec4 gold(0.71f, 0.68f, 0.52f, 1.0f);
    ImGui::TextColored(gold, "Co-op");
    ImGui::Separator();

    if (!s_Base) {
        ImGui::TextDisabled("Only for Elden Ring");
        return;
    }
    bool running = s_TaskRegistered && s_TaskSeen && !s_Faulted;
    if (s_Faulted) ImGui::TextUnformatted("Turned off after an error (see VinStyleRankGauge.log)");
    else if (s_TaskFailed) ImGui::TextDisabled("Not available on this game version (see VinStyleRankGauge.log)");
    else if (!running) ImGui::TextDisabled("Starting...");
    else if (!InSession(lobby)) ImGui::TextDisabled("Not in a co-op session");
    else if (view.empty()) ImGui::TextDisabled("No other players yet");

    for (const MemberView& m : view) {
        ImGui::Text("%s%s", m.name.c_str(), m.isHost ? "  (host)" : "");
        char farLabel[48];
        const char* label = StateLabel(m.state);
        if (m.state == State::FarAway && m.metres >= 0) {
            snprintf(farLabel, sizeof(farLabel), "far away (%d m)", m.metres);
            label = farLabel;
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(label).x);
        ImVec4 col = m.state == State::Visible ? ImVec4(0.62f, 0.78f, 0.52f, 1.0f)
                   : (m.state == State::Faded || m.state == State::BuiltWrong || m.state == State::Hidden) ? ImVec4(0.90f, 0.52f, 0.40f, 1.0f)
                   : ImVec4(0.75f, 0.72f, 0.62f, 1.0f);
        ImGui::TextColored(col, "%s", label);
    }

    // Resync: one press every few seconds; Reload and Go to: click twice. With the native menu up (Esc > System > Game
    // Options > Vin-Swag) F8 keeps only what changes while playing: the players, Go to, the invaders, messages
    static double resyncAt = -100.0, reloadArmedAt = -100.0, goArmedAt = -100.0;
    static uint64_t goArmedFor = 0;
    double t = ImGui::GetTime();
    bool session = running && InSession(lobby);
    bool native = NativeMenu::Active();
    float w = 200.0f * scale;
    ImGui::Spacing();
    if (!native) {
    ImGui::BeginDisabled(!session || t - resyncAt < 5.0);
    if (ImGui::Button("Resync players", ImVec2(w, 0))) {
        resyncAt = t;
        s_WantResync = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool armed = t - reloadArmedAt < 4.0;
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(armed ? "Click again to reload" : "Reload here", ImVec2(w, 0))) {
        if (armed) {
            reloadArmedAt = -100.0;
            s_WantReload = true;
        } else {
            reloadArmedAt = t;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Loading screen at your spot: rebuilds everyone,\nlike resting at a grace (enemies respawn too)");
    }
    // one per other player, two to a row
    int goButtons = 0;
    for (const MemberView& m : view) {
        if (!session || !m.steamId) continue;
        if (goButtons++ % 2) ImGui::SameLine();
        bool goArmed = goArmedFor == m.steamId && t - goArmedAt < 4.0;
        std::string label = goArmed ? "Click again to go" : m.isHost ? "Go to host" : "Go to " + m.name;
        if (ImGui::CalcTextSize(label.c_str()).x > w - ImGui::GetStyle().FramePadding.x * 2) label = "Go to them";
        ImGui::PushID((void*)(uintptr_t)m.steamId);
        if (ImGui::Button(label.c_str(), ImVec2(w, 0))) {
            if (goArmed) {
                goArmedAt = -100.0;
                s_WantGoTo = m.steamId;
            } else {
                goArmedAt = t;
                goArmedFor = m.steamId;
            }
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Loading screen, then you're next to %s\n(not with an invader on you, nor into a boss fight)",
                              m.isHost ? "the host" : m.name.c_str());
    }
    if (running && boss && !native) {
        if (ImGui::Button("Clear boss fight flag", ImVec2(w, 0))) s_WantClearBoss = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Your game thinks a boss fight is on. If none is (no boss bar),\n"
                              "this turns it off: graces and Reload / Go to work again");
    }
    // remembered across launches (Coop.ini); untick to turn it off if it ever misbehaves
    if (!native) {
    bool autoOn = s_AutoResync, posOn = s_PositionCheck;
    if (ImGui::Checkbox("Auto-resync", &autoOn)) {
        s_AutoResync = autoOn;
        SaveCoopSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Every 10 s, repairs a player you can't see, the way Resync players does.\n"
                          "Only when their game says they're alive, nearby and not loading\n"
                          "(no rebuilds in boss fights). Remembered; details in VinStyleRankGauge.log");
    ImGui::SameLine();
    ImGui::BeginDisabled(!autoOn);
    if (ImGui::Checkbox("Fix out-of-place players", &posOn)) {
        s_PositionCheck = posOn;
        SaveCoopSettings();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Also rebuild a player's character that stays more than 40 m\n"
                          "from where their own game says they are, for 6 s. Remembered");
    }

    // Invasions: the host (or a solo player) summons; everyone sees the list
    if (s_InvasionsOk && running && inGame) {
        ImGui::Spacing();
        ImGui::TextColored(gold, "Invasions");
        ImGui::Separator();
        for (const InvaderView& v : invaders) {
            ImGui::Text("%s", v.name.c_str());
            if (!v.title.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", v.title.c_str());
            }
            char hp[32];
            if (v.hp > 0) snprintf(hp, sizeof(hp), "%d / %d", v.hp, v.maxHp);
            else snprintf(hp, sizeof(hp), "defeated");
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(hp).x);
            ImGui::TextColored(v.hp > 0 ? ImVec4(0.90f, 0.40f, 0.35f, 1.0f) : ImVec4(0.62f, 0.78f, 0.52f, 1.0f), "%s", hp);
        }
        if (lobby == 6) {
            ImGui::TextDisabled("The host summons invaders");
        } else {
            if (!native) {
                if (ImGui::Button("Summon invader", ImVec2(w, 0))) s_WantInvader = true;
                if (s_Inv.autoOn) ImGui::SameLine();
            }
            if (s_Inv.autoOn) ImGui::TextDisabled("automatic: %.1f%% a roll now", s_ChanceNow * 100.0f);
        }
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::Spacing();
}

void DrawBanner(ImFont* font) {
    std::string line, title;
    ULONGLONG at;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        line = s_BannerLine;
        title = s_BannerTitle;
        at = s_BannerAt;
    }
    std::string reward;
    ULONGLONG rewardAt;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        reward = s_RewardText;
        rewardAt = s_RewardAt;
    }
    ImGuiIO& rio = ImGui::GetIO();
    ImFont* rf = font ? font : ImGui::GetFont();
    ULONGLONG rage = rewardAt ? GetTickCount64() - rewardAt : 0;
    if (rewardAt && rage < 5000) {
        float ra = rage < 300 ? rage / 300.0f : rage > 4000 ? (5000 - rage) / 1000.0f : 1.0f;
        float size = floorf(30.0f * rio.DisplaySize.y / 1080.0f);
        std::string text = "Spoils: " + reward;
        ImVec2 ts = rf->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        ImVec2 pos(floorf(rio.DisplaySize.x * 0.5f - ts.x * 0.5f), rio.DisplaySize.y * 0.40f);
        ImDrawList* rdl = ImGui::GetForegroundDrawList();
        rdl->AddText(rf, size, ImVec2(pos.x + 2.0f, pos.y + 2.0f), IM_COL32(0, 0, 0, (int)(ra * 255)), text.c_str());
        rdl->AddText(rf, size, pos, IM_COL32(222, 196, 120, (int)(ra * 255)), text.c_str());
    }
    ULONGLONG age = at ? GetTickCount64() - at : 0;
    if (!at || age > 5000) return;
    float alpha = age < 300 ? age / 300.0f : age > 4000 ? (5000 - age) / 1000.0f : 1.0f;
    ImGuiIO& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    ImFont* f = font ? font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float bigSize = floorf(46.0f * scale), titleSize = floorf(24.0f * scale);
    ImVec2 s1 = f->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, line.c_str());
    ImVec2 s2 = f->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title.c_str());
    float y = io.DisplaySize.y * 0.26f;
    float bandH = s1.y + (title.empty() ? 0.0f : s2.y + 6.0f * scale) + 28.0f * scale;
    int a = (int)(alpha * 255.0f);
    dl->AddRectFilled(ImVec2(0, y - 14.0f * scale), ImVec2(io.DisplaySize.x, y - 14.0f * scale + bandH), IM_COL32(0, 0, 0, a * 150 / 255));
    ImVec2 p1(floorf(io.DisplaySize.x * 0.5f - s1.x * 0.5f), y);
    dl->AddText(f, bigSize, ImVec2(p1.x + 2.0f, p1.y + 2.0f), IM_COL32(0, 0, 0, a), line.c_str());
    dl->AddText(f, bigSize, p1, IM_COL32(205, 60, 50, a), line.c_str());
    if (!title.empty()) {
        ImVec2 p2(floorf(io.DisplaySize.x * 0.5f - s2.x * 0.5f), y + s1.y + 6.0f * scale);
        dl->AddText(f, titleSize, p2, IM_COL32(200, 188, 150, a), title.c_str());
    }
}

} // namespace CoopResync
