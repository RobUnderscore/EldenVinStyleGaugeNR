#include "coop_resync.h"
#include "imgui.h"
#include "memory.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
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
            if (from && data && size == (int)sizeof(msg)) {
                memcpy(&msg, data, sizeof(msg));
                if (msg.magic == kSpotMagic && msg.version == kSpotVersion) {
                    Heard h = { from, { msg.block, msg.x, msg.y, msg.z, msg.yaw }, msg.flags, now };
                    bool found = false;
                    for (Heard& old : s_Heard)
                        if (old.steamId == from) { old = h; found = true; }
                    if (!found) {
                        s_Heard.push_back(h);
                        Log("CoopResync: link: hearing from %llX (at %08X)", (unsigned long long)from, (uint32_t)msg.block);
                    }
                }
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
    int lobby;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        view = s_View;
        lobby = s_ViewLobby;
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
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::Spacing();
}

} // namespace CoopResync
