#include "coop_resync.h"
#include "convenience_menu.h"
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
    std::string name;
    bool isHost;
    State state;
    int metres;  // -1 unknown
};

static std::mutex s_Lock;
static std::vector<MemberView> s_View;  // other players
static int s_ViewLobby = 0;
static bool s_ViewInGame = false;
static std::string s_Message;
static ULONGLONG s_MessageAt = 0;

static std::atomic<bool> s_TaskRegistered{ false }, s_TaskFailed{ false }, s_TaskSeen{ false }, s_Faulted{ false };
static std::atomic<bool> s_WantResync{ false }, s_WantReload{ false }, s_WantHost{ false };

static void SetMessage(const std::string& msg) {
    Log("CoopResync: %s", msg.c_str());
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Message = msg;
    s_MessageAt = GetTickCount64();
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
            view.push_back({ NameOf(m, mine), m.isHost, st, (int)d });
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_View = std::move(view);
    s_ViewLobby = main ? lobby : 0;
    s_ViewInGame = main != 0;
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
};
static std::vector<Revived> s_Revived;
static ULONGLONG s_FollowUpAt = 0;

static const Copy* CopyOf(const Member& m, const std::vector<Member>& members, const std::vector<Copy>& set, uintptr_t main) {
    for (const Copy& c : set)
        if (c.chr != main && OwnerOf(c.chr, members) == &m) return &c;
    return nullptr;
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
        case State::NoCharacter:
            // a copy we couldn't match may be theirs: building a second one would be worse
            if (unmatched) line = "no character found, try Reload here";
            else {
                *reinterpret_cast<volatile uint8_t*>(m.entry + 0xea) = 1;
                Log("CoopResync: %llX had no copy: join_wait set", (unsigned long long)m.steamId);
                line = "loading their character";
            }
            break;
        case State::Faded:
            if (Revive(c->chr)) {
                s_Revived.push_back({ m.steamId, name });
                line = "brought back";
                break;
            }
            [[fallthrough]];
        case State::Hidden:
        case State::NotLoaded:
        case State::BuiltWrong: {
            std::string why = Rebuild(m, c->chr, lobby);
            line = why.empty() ? "rebuilding their character" : why;
            break;
        }
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
    std::string report;
    for (const Revived& r : s_Revived) {
        for (const Member& m : members) {
            if (m.isLocal || m.steamId != r.steamId) continue;
            const Copy* c = CopyOf(m, members, set, main);
            State st = c ? Classify(*c, main, now) : State::NoCharacter;
            // dead again (they really are), far away or fine: a rebuild now would only make it worse
            if (!c || (st != State::Hidden && st != State::Faded && st != State::BuiltWrong && st != State::NotLoaded)) break;
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
static void ReloadAt(const Spot& spot, const char* what) {
    Log("CoopResync: %s: block %08X (%.2f, %.2f, %.2f) yaw %.2f", what, (uint32_t)spot.block, spot.x, spot.y, spot.z, spot.yaw);
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
    reload();
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

// ---- the overlay link ----
// Our own Steam messages between the party's overlays. Seamless uses channels 40 and 80 and
// accepts every peer's session, so a channel of our own reaches the same players. Each overlay
// tells the others where it stands about once a second; a far-off player's character here has no
// position at all (local path, every block -1), so this is how a guest learns where the host is.

static constexpr int kLinkChannel = 0x5753;
static constexpr uint32_t kSpotMagic = 0x53575356;  // "VSWS"
static constexpr uint16_t kSpotVersion = 1;
static constexpr ULONGLONG kSpotEveryMs = 1000, kSpotFreshMs = 5000;
enum : uint16_t { kSpotDead = 1, kSpotLoading = 2 };

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
    Spot spot;
    uint16_t flags;
    ULONGLONG at;
};
static std::vector<Heard> s_Heard;
static std::vector<uint64_t> s_SendLogged;
static ULONGLONG s_NextSpotSend = 0;

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
    if (now < s_NextSpotSend) return;
    s_NextSpotSend = now + kSpotEveryMs;
    void* net = s_NetMessages();
    if (!net) return;
    std::vector<Member> members = Members();
    uintptr_t main = MainPlayer(), gm = Global(GAME_MAN);
    SpotMsg msg = { kSpotMagic, kSpotVersion, 0, -1, 0.0f, 0.0f, 0.0f, 0.0f };
    Spot spot;
    if (main && OurSpot(main, spot)) {
        msg.block = spot.block;
        msg.x = spot.x, msg.y = spot.y, msg.z = spot.z, msg.yaw = spot.yaw;
    }
    if (!main || (gm && Rd<uint8_t>(gm + 0x10))) msg.flags |= kSpotLoading;
    if (main && Hp(main) <= 0) msg.flags |= kSpotDead;
    for (const Member& m : members) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        int result = s_Send(net, &to, &msg, sizeof(msg), 32 /* unreliable, restart broken sessions */, kLinkChannel);
        bool logged = false;
        for (uint64_t id : s_SendLogged) logged |= id == m.steamId;
        if (!logged) {
            s_SendLogged.push_back(m.steamId);
            Log("CoopResync: link: first spot sent to %llX (result %d)", (unsigned long long)m.steamId, result);
        }
    }
}

static void OnLinkMessage(uint64_t from, const uint8_t* data, int size, ULONGLONG now);  // invasions

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
            if (from && data && size == (int)sizeof(msg) && Rd<uint32_t>(reinterpret_cast<uintptr_t>(data)) == kSpotMagic) {
                memcpy(&msg, data, sizeof(msg));
                if (msg.version == kSpotVersion) {
                    Heard h = { from, { msg.block, msg.x, msg.y, msg.z, msg.yaw }, msg.flags, now };
                    bool found = false;
                    for (Heard& old : s_Heard)
                        if (old.steamId == from) { old = h; found = true; }
                    if (!found) {
                        s_Heard.push_back(h);
                        Log("CoopResync: link: hearing from %llX (at %08X)", (unsigned long long)from, (uint32_t)msg.block);
                    }
                }
            } else if (from && data && size > 8) {
                OnLinkMessage(from, data, size, now);
            }
            s_Release(msgs[i]);
        }
        if (n < 16) break;
    }
}

// Guests: reload at the host. In order: the host's own report over the link, their character's
// live position here (when it has its block origin), the spot the game recorded when we joined.
// All three are logged, with a check of SpotOf against the game's own numbers for our character.
static void GoToHost(ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    if (lobby != 6) return SetMessage("Only for guests: you're the host or not in co-op");
    std::string no = ReloadRefusal(main);
    if (!no.empty()) return SetMessage(no);

    std::vector<Member> members = Members();
    const Member* host = nullptr;
    for (const Member& m : members)
        if (m.isHost && !m.isLocal) host = &m;
    if (!host) return SetMessage("Can't find the host in the session");
    std::vector<Copy> set = PlayerSet();
    const Copy* c = CopyOf(*host, members, set, main);
    const Heard* told = HeardFrom(host->steamId, now);
    if (told && (told->flags & kSpotDead)) return SetMessage("The host is down, try again once they've respawned");
    if (told && (told->flags & kSpotLoading)) return SetMessage("The host is loading, try again in a moment");
    if (!told && c && IsDead(c->chr)) return SetMessage("The host is down, try again once they've respawned");

    Spot live = {}, joined = {}, self = {};
    bool haveTold = told && told->spot.Valid();
    bool haveLive = c && SpotOf(c->chr, live);
    bool haveJoined = BreakInHostSpot(joined);
    if (SpotOf(main, self))
        Log("CoopResync: spot check on us: worked out %08X (%.2f, %.2f, %.2f) yaw %.2f, game has %08X (%.2f, %.2f, %.2f) yaw %.2f",
            (uint32_t)self.block, self.x, self.y, self.z, self.yaw, Rd<uint32_t>(main + 0x6d0), Rd<float>(main + 0x6c0),
            Rd<float>(main + 0x6c4), Rd<float>(main + 0x6c8), Rd<float>(main + 0x6cc));
    if (haveTold)
        Log("CoopResync: host's own report: %08X (%.2f, %.2f, %.2f)", (uint32_t)told->spot.block, told->spot.x, told->spot.y, told->spot.z);
    if (haveLive)
        Log("CoopResync: host from their character: %08X (%.2f, %.2f, %.2f)", (uint32_t)live.block, live.x, live.y, live.z);
    if (haveJoined)
        Log("CoopResync: host from the join record: %08X (%.2f, %.2f, %.2f)", (uint32_t)joined.block, joined.x, joined.y, joined.z);

    if (haveTold) {
        ReloadAt(told->spot, "go to host (their report)");
    } else if (haveLive) {
        ReloadAt(live, "go to host (their character)");
    } else if (haveJoined) {
        ReloadAt(joined, "go to host (join record)");
    } else {
        return SetMessage("Can't tell where the host is: are they on the latest pack?");
    }
    SetMessage("Going to the host...");
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
// Param tables: SoloParamRepository [0x3d88f78]; table i's ResCap at +0x88 + 0x48 i (name at +0x18), its param
// file at [[ResCap+0x80]+0x80]: row count u16 at +0xa, rows from +0x40, 0x18 bytes each {id, pad, data offset}
static constexpr uintptr_t SOLO_PARAM_REPO = 0x3d88f78;
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
};
static InvasionSettings s_Inv;

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

// The host's invasion between sending the armor and summoning
struct PendingInvasion {
    bool on = false;
    const InvaderEntry* pick = nullptr;
    std::string name;
    int bucket = 0;
    bool automatic = false;
    ULONGLONG at = 0;
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
static NameSlot s_NameSlots[96];
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
    int scaledId = 0;          // its toughness SpEffect on this game
    ULONGLONG since = 0;       // first seen here
    ULONGLONG topUpAt = 0;     // own invader: fill HP to the scaled maximum then
    bool named = false;        // name and title are the summoner's (not a placeholder)
    bool nameShown = false;    // the game's name lookup gives it
    bool looked = false;       // logged its look
};
static std::vector<ActiveInvader> s_Active;

#pragma pack(push, 1)
struct InvaderMsg {
    uint32_t magic;
    uint16_t version;  // 2: bucket is the level catch-up step
    uint16_t bucket;
    uint64_t handle;   // FieldInsHandle, the same on every game
    char name[24];
    char title[40];
};
// Sent before the summon: every game dresses the build's CharaInitParam row the same way
struct ArmorMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t chara;
    int32_t armor[4];
};
#pragma pack(pop)

// The menu's view of them, and the banner
struct InvaderView {
    std::string name, title;
    int hp, maxHp;
};
static std::vector<InvaderView> s_InvaderView;
static std::string s_BannerName, s_BannerTitle, s_RewardText;
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

// The dismissal check's lookup (game thread): the ghost owner is present while an invader lives
static uintptr_t DismissLookup(uintptr_t wcm, int32_t number) {
    if (number == kGhostOwner) {
        ULONGLONG last = s_LastInvaderAlive.load();
        return last && GetTickCount64() - last < kGhostLingerMs ? wcm : 0;
    }
    return reinterpret_cast<uintptr_t (*)(uintptr_t, int32_t)>(s_PrevDismissLookup)(wcm, number);
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
        }
        fclose(f);
    }
    Log("CoopResync: invasions: %zu invaders, %zu names, %zu loot items, armor %zu/%zu/%zu/%zu; auto %d every %ds, certain "
        "after %.0f min (power %.1f), loot %d%%, arrive %.0f-%.0f m, look %d, random armor %d, level margin %d, scaling %d "
        "step %d to %d", s_InvaderList.size(), s_InvaderNames.size(), s_Loot.size(), s_Armor[0].size(), s_Armor[1].size(),
        s_Armor[2].size(), s_Armor[3].size(), s_Inv.autoOn, s_Inv.checkSeconds, s_Inv.guaranteeMinutes, s_Inv.rampPower,
        s_Inv.lootChance, s_Inv.arriveMin, s_Inv.arriveMax, s_Inv.look, s_Inv.randomArmor, s_Inv.levelMargin, s_ScaleBase,
        s_ScaleStep, s_ScaleLast);
    for (const InvaderEntry& e : s_InvaderList) SetInvaderName(e.npc, e.title);
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

static void ShowBanner(const std::string& name, const std::string& title) {
    std::lock_guard<std::mutex> lock(s_Lock);
    s_BannerName = name;
    s_BannerTitle = title;
    s_BannerAt = GetTickCount64();
}

static void SendInvader(const ActiveInvader& a) {
    void* net = LinkOk() ? s_NetMessages() : nullptr;
    if (!net) return;
    InvaderMsg msg = {};
    msg.magic = kInvaderMagic;
    msg.version = 2;
    msg.bucket = static_cast<uint16_t>(a.bucket < 0 ? 0 : a.bucket);
    msg.handle = a.handle;
    strncpy(msg.name, a.name.c_str(), sizeof(msg.name) - 1);
    strncpy(msg.title, a.title.c_str(), sizeof(msg.title) - 1);
    for (const Member& m : Members()) {
        if (m.isLocal || !m.steamId) continue;
        SteamIdentity to = { 16, 8, m.steamId, {} };
        s_Send(net, &to, &msg, sizeof(msg), 32, kLinkChannel);
    }
}

// A build's CharaInitParam row in this game's memory (0 if the table isn't as expected)
static uintptr_t CharaRow(int32_t id) {
    uintptr_t repo = Global(SOLO_PARAM_REPO);
    if (!repo || !Rd<uint32_t>(repo + 0x80 + kCharaInitTable * 0x48)) return 0;
    uintptr_t cap = Ptr(repo + 0x88 + kCharaInitTable * 0x48);
    uintptr_t name = cap ? Ptr(cap + 0x18) : 0;
    if (!name || wcscmp(reinterpret_cast<const wchar_t*>(name), L"CharaInitParam") != 0) return 0;
    uintptr_t file = Ptr(Ptr(cap + 0x80) + 0x80);
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
    if (size == (int)sizeof(ArmorMsg)) {
        ArmorMsg am;
        memcpy(&am, data, sizeof(am));
        if (am.magic != kArmorMagic || am.version != 1) return;
        bool ok = am.chara >= 3910000 && am.chara <= 3919999 && DressRow(am.chara, am.armor);
        Log("CoopResync: invasions: %llX dresses %d in %d/%d/%d/%d: %s", (unsigned long long)from, am.chara, am.armor[0],
            am.armor[1], am.armor[2], am.armor[3], ok ? "done" : "NOT done");
        return;
    }
    InvaderMsg msg;
    if (size != (int)sizeof(msg)) return;
    memcpy(&msg, data, sizeof(msg));
    if (msg.magic != kInvaderMagic || (msg.version != 1 && msg.version != 2)) return;
    msg.name[sizeof(msg.name) - 1] = 0;
    msg.title[sizeof(msg.title) - 1] = 0;
    ActiveInvader* a = ActiveByHandle(msg.handle);
    if (a && a->named) return;
    if (!a) {
        s_Active.push_back({ msg.handle, "", "", now, 0, 0 });
        a = &s_Active.back();
    }
    a->name = msg.name;
    a->title = msg.title;
    a->named = true;
    a->bucket = msg.version >= 2 ? msg.bucket : 0;
    Log("CoopResync: invasions: %llX says %llX is %s (%s), level step %d", (unsigned long long)from,
        (unsigned long long)msg.handle, msg.name, msg.title, a->bucket);
    ShowBanner(msg.name, msg.title);
}

static bool CallSummon(uintptr_t man) {
    __try {
        reinterpret_cast<void (*)(uintptr_t)>(Addr(SUMMON_REQUEST))(man);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Host (or solo): pick one invader for the host's level, dress its row in random armor on every game (the
// others are told first and the summon waits a moment, as each game builds its copy from its own row)
static void SummonInvader(ULONGLONG now, bool automatic) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer(), wcm = Global(WORLD_CHR_MAN);
    uintptr_t man = wcm ? Ptr(wcm + 0x1e538) : 0;
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    if (!s_InvasionsOk || !man || !pgd || !Intact(SUMMON_REQUEST, true)) return SetMessage("Invasions aren't available");
    if (lobby == 6) return SetMessage("Only the host can summon invaders");
    if (s_Pending.on) return SetMessage("An invader is on the way");
    if (const char* why = InvasionBlocked(main)) return SetMessage(std::string("No invasion now: ") + why);
    int level = Rd<int32_t>(pgd + 0x68);
    std::vector<const InvaderEntry*> fit;
    for (const InvaderEntry& e : s_InvaderList)
        if (level >= e.levelMin && level <= e.levelMax && e.level <= level + s_Inv.levelMargin) fit.push_back(&e);
    if (fit.empty()) return SetMessage("No invader fits your level");
    const InvaderEntry& pick = *fit[Random((uint32_t)fit.size())];
    s_Pending.on = true;
    s_Pending.pick = &pick;
    s_Pending.name = s_InvaderNames.empty() ? pick.title : s_InvaderNames[Random((uint32_t)s_InvaderNames.size())];
    s_Pending.bucket = s_ScaleStep > 0 ? (std::min)((std::max)(level - pick.level, 0) / s_ScaleStep, s_ScaleLast) : 0;
    s_Pending.automatic = automatic;
    s_Pending.at = now;
    bool others = InSession(lobby) && Members().size() > 1;
    if (s_Inv.randomArmor && s_Armor[0].size() && s_Armor[1].size() && s_Armor[2].size() && s_Armor[3].size()) {
        int32_t armor[4];
        std::string names;
        for (int i = 0; i < 4; i++) {
            const ArmorPiece& a = s_Armor[i][Random((uint32_t)s_Armor[i].size())];
            armor[i] = a.id;
            names += (i ? ", " : "") + a.name;
        }
        bool dressed = DressRow(pick.chara, armor);
        if (others) {
            SendArmor(pick.chara, armor);
            s_Pending.at = now + kArmorLeadMs;
        }
        Log("CoopResync: invasions: %s dressed in %s%s", pick.title.c_str(), names.c_str(), dressed ? "" : " (row NOT found)");
    }
}

static void SummonPending(ULONGLONG now) {
    PendingInvasion p = s_Pending;
    s_Pending = {};
    uintptr_t main = MainPlayer(), wcm = Global(WORLD_CHR_MAN);
    uintptr_t man = wcm ? Ptr(wcm + 0x1e538) : 0;
    uintptr_t pgd = main ? Ptr(main + 0x580) : 0;
    if (!man || !pgd) return SetMessage("Invasions aren't available");
    if (const char* why = InvasionBlocked(main)) return SetMessage(std::string("No invasion now: ") + why);
    const InvaderEntry& pick = *p.pick;
    const std::string& name = p.name;
    bool automatic = p.automatic;
    int level = Rd<int32_t>(pgd + 0x68);

    alignas(16) uint8_t buf[0x20] = {};
    const float* phys = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(PHYS_POS))(main, buf);
    alignas(16) uint8_t rbuf[0x20] = {};
    const float* rot = reinterpret_cast<const float* (*)(uintptr_t, void*)>(Addr(ROTATION))(main, rbuf);
    if (!phys || !rot) return SetMessage("Couldn't tell where you are");
    std::vector<uint64_t> before;
    for (uintptr_t c : InvaderChrs()) before.push_back(Rd<uint64_t>(c + 8));

    // they arrive a little way back along the host's path (or right here if there's no such spot)
    alignas(16) float origin[4] = { phys[0], phys[1], phys[2], phys[3] };
    float away = ArrivalPoint(main, now, origin);
    // the summon manager summons for the main player: give it the ghost's number for this one call
    int32_t number = Rd<int32_t>(pgd + 8), req = Rd<int32_t>(man + 0x20), active = Rd<int32_t>(man + 0x24);
    memcpy(reinterpret_cast<void*>(man + 0xa0), origin, sizeof(origin));
    *reinterpret_cast<volatile float*>(man + 0xb0) = rot[1];
    *reinterpret_cast<volatile int32_t*>(man + 0x20) = pick.buddy;
    *reinterpret_cast<volatile int32_t*>(pgd + 8) = kGhostOwner;
    bool ok = CallSummon(man);
    *reinterpret_cast<volatile int32_t*>(pgd + 8) = number;
    *reinterpret_cast<volatile int32_t*>(man + 0x20) = req;
    *reinterpret_cast<volatile int32_t*>(man + 0x24) = active;
    s_LastInvaderAlive = now;
    if (!ok) return SetMessage("The summon failed (see the log)");

    int found = 0;
    for (uintptr_t c : InvaderChrs()) {
        uint64_t h = Rd<uint64_t>(c + 8);
        bool old = false;
        for (uint64_t b : before) old |= b == h;
        if (old || ActiveByHandle(h)) continue;
        *reinterpret_cast<volatile uint8_t*>(c + 0x6c) = kInvaderTeam;
        ActiveInvader a = { h, name, pick.title, now, 3, now };
        a.own = true;
        a.named = true;
        a.bucket = p.bucket;
        s_Active.push_back(a);
        found++;
    }
    s_LastInvasion = now;
    Log("CoopResync: invasions: %s %s (%s, buddy %d, invader level %d, host level %d, catch-up step %d, %s): %d character(s)",
        automatic ? "automatic" : "summoned", name.c_str(), pick.title.c_str(), pick.buddy, pick.level, level, p.bucket,
        away > 0 ? (std::to_string((int)away) + " m away").c_str() : "right here", found);
    if (!found) return SetMessage("The summon made no invader (see the log)");
    ShowBanner(name, pick.title);
    SetMessage("Invaded by " + name + "!");
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
    if (!a.looked) {
        a.looked = true;
        Log("CoopResync: invasions: %s in PhantomParam %d (%s; npc %d, team %d)", a.name.c_str(), phantom,
            a.own ? "ours: phantom override" : painted ? "another game's: painted" : "another game's: NOT painted",
            Rd<int32_t>(c + 0x60), Rd<uint8_t>(c + 0x6c));
    }
}

// Every game, every frame: invaders hostile, scaled to the party, in the invader look and named; the
// summoner's game keeps the ghost owner present while one lives; announcements go out a few times
// (messages are unreliable); each game rolls its player's loot when an invader dies; the host's game
// runs the automatic invasions; invaders gone for a while are forgotten
static void InvasionFrame(ULONGLONG now) {
    int lobby = LobbyState();
    uintptr_t main = MainPlayer();
    int party = InSession(lobby) ? (int)Members().size() : 1;
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
        }
        a->seen = now;
        if (a->named && !a->nameShown) {
            SetInvaderName(Rd<int32_t>(c + 0x60), a->name);
            a->nameShown = true;
        }
        int hp = Hp(c);
        uintptr_t mods = Ptr(c + 0x190);
        uintptr_t data = mods ? Ptr(mods) : 0;
        int maxHp = data ? Rd<int32_t>(data + 0x13c) : 0;
        if (hp > 0) {
            alive = true;
            s_LastInvaderAlive = now;
            if (s_Inv.look > 0) PaintInvader(c, *a, s_Inv.look);
            // toughness = party size x level catch-up; another game's invader waits up to 3 s for its step
            if (!a->since) a->since = now;
            int bucket = a->bucket >= 0 ? a->bucket : now - a->since > 3000 ? 0 : -1;
            int32_t want = s_ScaleBase && bucket >= 0 ? s_ScaleBase + 1000 * (std::min)(party, 6) + (std::min)(bucket, s_ScaleLast) : 0;
            if (want && want != a->scaledId && Intact(APPLY_SPEFFECT, true) && Intact(REMOVE_SPEFFECT, true)) {
                if (a->scaledId) reinterpret_cast<void (*)(uintptr_t, int32_t)>(Addr(REMOVE_SPEFFECT))(c, a->scaledId);
                reinterpret_cast<uint64_t (*)(uintptr_t, int32_t, uint8_t)>(Addr(APPLY_SPEFFECT))(c, want, 1);
                Log("CoopResync: invasions: %s scaled for %d player(s), level step %d (SpEffect %d)", a->name.c_str(), party,
                    bucket, want);
                a->scaledId = want;
                if (a->own) a->topUpAt = now + 500;
            }
            if (a->topUpAt && now >= a->topUpAt && data) {
                *reinterpret_cast<volatile int32_t*>(data + 0x138) = maxHp;
                a->topUpAt = 0;
            }
        } else if (!a->rewarded) {
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
    for (ActiveInvader& a : s_Active) {
        if (a.sends > 0 && now >= a.nextSend) {
            SendInvader(a);
            a.sends--;
            a.nextSend = now + 1000;
        }
    }
    for (size_t i = 0; i < s_Active.size();)
        if (now - s_Active[i].seen > 30000) s_Active.erase(s_Active.begin() + i);
        else i++;

    // automatic invasions: the host (or a solo player) rolls every checkSeconds; the chance climbs
    // from 0 to certain over guaranteeMinutes since the last invasion
    if (s_Inv.autoOn && lobby != 6 && main) {
        if (!s_LastInvasion) s_LastInvasion = now;
        float minutes = (now - s_LastInvasion) / 60000.0f;
        s_ChanceNow = fminf(1.0f, powf(minutes / fmaxf(1.0f, s_Inv.guaranteeMinutes), s_Inv.rampPower));
        if (now >= s_NextCheck) {
            s_NextCheck = now + (ULONGLONG)fmaxf(5.0f, (float)s_Inv.checkSeconds) * 1000;
            if (!alive && !s_Pending.on && Random(1000000) < (uint32_t)(s_ChanceNow * 1000000.0f)) {
                const char* why = InvasionBlocked(main);
                if (why) Log("CoopResync: invasions: an automatic invasion is due but waits (%s)", why);
                else SummonInvader(now, true);
            }
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_InvaderView = std::move(view);
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
    if (s_WantHost.exchange(false)) GoToHost(now);
    if (s_InvasionsOk) {
        if (s_WantInvader.exchange(false)) SummonInvader(now, false);
        if (s_Pending.on && now >= s_Pending.at) SummonPending(now);
        InvasionFrame(now);
    }
    if (s_FollowUpAt && now >= s_FollowUpAt) {
        s_FollowUpAt = 0;
        FollowUp(now);
    }
    if (now >= s_NextSnapshot) {
        s_NextSnapshot = now + 250;
        Snapshot();
    }
}

static void TaskExecute(void*, const void*) {
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

void DrawMenuSection(float scale) {
    std::vector<MemberView> view;
    std::vector<InvaderView> invaders;
    int lobby;
    bool inGame;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        view = s_View;
        invaders = s_InvaderView;
        lobby = s_ViewLobby;
        inGame = s_ViewInGame;
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

    // Resync: one press every few seconds; Reload and Go to host: click twice
    static double resyncAt = -100.0, reloadArmedAt = -100.0, hostArmedAt = -100.0;
    double t = ImGui::GetTime();
    bool session = running && InSession(lobby);
    float w = 200.0f * scale;
    ImGui::Spacing();
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
    if (session && lobby == 6) {
        bool hostArmed = t - hostArmedAt < 4.0;
        if (ImGui::Button(hostArmed ? "Click again to go" : "Go to host", ImVec2(w, 0))) {
            if (hostArmed) {
                hostArmedAt = -100.0;
                s_WantHost = true;
            } else {
                hostArmedAt = t;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loading screen, then you're next to the host");
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
            if (ImGui::Button("Summon invader", ImVec2(w, 0))) s_WantInvader = true;
            if (s_Inv.autoOn) {
                ImGui::SameLine();
                ImGui::TextDisabled("automatic: %.1f%% a roll now", s_ChanceNow * 100.0f);
            }
        }
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::Spacing();
}

void DrawBanner(ImFont* font) {
    std::string name, title;
    ULONGLONG at;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        name = s_BannerName;
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
    std::string line = "Invaded by " + name + "!";
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
