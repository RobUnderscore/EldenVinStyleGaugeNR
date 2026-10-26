#include "memory.h"
#include <Windows.h>
#include <Psapi.h>
#include <Pattern16.h>

#pragma comment(lib, "psapi.lib")

namespace Memory {

static uintptr_t g_WorldChrMan = 0;
static const GameLayout* g_Game = nullptr;

static const GameLayout kGames[] = {
    // Nightreign
    { "nightreign.exe", "48 8B 05 ?? ?? ?? ?? 0F 28 F1 48 85 C0",
      0x174e8, 0x190, 0x38, 0x48, 0 },
    // Elden Ring 2.7.x (offsets from fromsoftware-rs; AOB resolves to the static at +0x3d6d018)
    { "eldenring.exe", "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88",
      0x1e508, 0x178, 0x30, 0x40, 999320800 },
};

const GameLayout* DetectGame() {
    for (const auto& g : kGames) {
        if (GetModuleHandleA(g.moduleName)) {
            g_Game = &g;
            return g_Game;
        }
    }
    return nullptr;
}

const GameLayout* Game() {
    return g_Game;
}

bool Init() {
    if (!g_Game) return false;
    g_WorldChrMan = AOBScan(g_Game->worldChrManAOB, g_Game->moduleName);
    if (g_WorldChrMan == 0) return false;

    // Resolve RIP-relative address
    int32_t displacement = *reinterpret_cast<int32_t*>(g_WorldChrMan + 3);
    g_WorldChrMan = g_WorldChrMan + 7 + displacement;

    return g_WorldChrMan != 0;
}

uintptr_t GetWorldChrMan() {
    if (g_WorldChrMan == 0) return 0;
    return Read<uintptr_t>(g_WorldChrMan);
}

uintptr_t GetChrIns() {
    uintptr_t wcm = GetWorldChrMan();
    if (wcm == 0) return 0;
    return Read<uintptr_t>(wcm + g_Game->mainPlayer);
}

uintptr_t GetSpEffectCtrl() {
    uintptr_t chrIns = GetChrIns();
    if (chrIns == 0) return 0;
    return Read<uintptr_t>(chrIns + g_Game->specialEffect);
}

uintptr_t GetSpEffectListHead() {
    uintptr_t ctrl = GetSpEffectCtrl();
    if (ctrl == 0) return 0;
    return Read<uintptr_t>(ctrl + 0x08);
}

uintptr_t AOBScan(const char* pattern, const char* moduleName) {
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) return 0;

    MODULEINFO modInfo{};
    GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo));

    auto result = Pattern16::scan(
        reinterpret_cast<void*>(hModule),
        modInfo.SizeOfImage,
        std::string(pattern)
    );

    return reinterpret_cast<uintptr_t>(result);
}

} // namespace Memory
