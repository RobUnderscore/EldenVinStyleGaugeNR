#include "memory.h"
#include <Windows.h>
#include <Psapi.h>
#include <Pattern16.h>

#pragma comment(lib, "psapi.lib")

namespace Memory {

static uintptr_t g_WorldChrMan = 0;

bool Init() {
    g_WorldChrMan = AOBScan("48 8B 05 ?? ?? ?? ?? 0F 28 F1 48 85 C0");
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
    return Read<uintptr_t>(wcm + 0x174e8);
}

uintptr_t GetSpEffectCtrl() {
    uintptr_t chrIns = GetChrIns();
    if (chrIns == 0) return 0;
    return Read<uintptr_t>(chrIns + 0x190);
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
