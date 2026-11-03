#include "hud_icons.h"
#include "memory.h"
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace HudIcons {

static constexpr uintptr_t STRIP_UPDATE = 0x8d58f0;  // (strip parts, dt in xmm1, icon list)
static constexpr uint8_t kStripUpdateBytes[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x55, 0x41, 0x57,
                                                 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9, 0xba };
static constexpr uintptr_t STRIP_VISIBLE = 0xaaa;     // icon list: u8, the strip is shown

using StripUpdate = uint64_t (*)(void*, float, void*, void*);
static StripUpdate s_Orig = nullptr;
static std::atomic<uintptr_t> s_List{ 0 };
static std::atomic<uint64_t> s_Seen{ 0 };  // GetTickCount64 of the last update
static bool s_Tried = false;

static uint64_t HkStripUpdate(void* parts, float dt, void* list, void* r9) {
    s_List.store(reinterpret_cast<uintptr_t>(list), std::memory_order_relaxed);
    s_Seen.store(GetTickCount64(), std::memory_order_release);
    return s_Orig(parts, dt, list, r9);
}

void Install() {
    if (s_Tried) return;
    s_Tried = true;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) return;
    uint8_t* site = reinterpret_cast<uint8_t*>(base + STRIP_UPDATE);
    // another mod may have put a jump at its start
    bool hooked = site[0] == 0xE9 && !memcmp(site + 5, kStripUpdateBytes + 5, sizeof(kStripUpdateBytes) - 5);
    if (!hooked && memcmp(site, kStripUpdateBytes, sizeof(kStripUpdateBytes)) != 0) {
        Log("HudIcons: the icon-strip update is not as expected; buff times stay in their own row");
        return;
    }
    MH_STATUS st = MH_CreateHook(site, reinterpret_cast<void*>(&HkStripUpdate), reinterpret_cast<void**>(&s_Orig));
    if (st == MH_OK) st = MH_EnableHook(site);
    Log("HudIcons: icon-strip hook %d%s", st, hooked ? " (after another mod's)" : "");
}

int Icons(int32_t (&ids)[kSlots]) {
    uint64_t seen = s_Seen.load(std::memory_order_acquire);
    uintptr_t list = s_List.load(std::memory_order_relaxed);
    if (!list || GetTickCount64() - seen > 500) return 0;
    if (!Memory::Read<uint8_t>(list + STRIP_VISIBLE)) return 0;
    int n = 0;
    for (; n < kSlots; n++) {
        float id = Memory::Read<float>(list + n * 0x40 + 0x3c);
        if (!(id >= 1.0f && id <= 99999.0f)) break;
        ids[n] = (int32_t)id;
    }
    return n;
}

} // namespace HudIcons
