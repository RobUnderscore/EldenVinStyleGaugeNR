#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Memory {

// Per-game addresses. The style SpEffect IDs are the same in both Elden Vins
// builds; only the exe, the WorldChrMan lookup and the struct offsets differ.
struct GameLayout {
    const char* moduleName;
    const char* worldChrManAOB;  // "48 8B 05 <rel32> ..." load of the WorldChrMan static
    ptrdiff_t   mainPlayer;      // WorldChrMan -> main player ChrIns
    ptrdiff_t   specialEffect;   // ChrIns -> CSSpecialEffect (list head at +0x08)
    ptrdiff_t   nodeNext;        // SpEffect node -> next node
    ptrdiff_t   nodeRemaining;   // SpEffect node -> remaining duration (float)
    int32_t     ultEffectId;     // accumulator SpEffect behind the Ultimate Art (0 = none)
};

// Find which supported game this process is (nullptr if neither is loaded yet)
const GameLayout* DetectGame();

// The detected game's layout (nullptr before DetectGame succeeds)
const GameLayout* Game();

// Initialize - find WorldChrMan via AOB scan
bool Init();

// Get base addresses (returns 0 if not found)
uintptr_t GetWorldChrMan();
uintptr_t GetChrIns();
uintptr_t GetSpEffectCtrl();
uintptr_t GetSpEffectListHead();

// AOB scan helper
uintptr_t AOBScan(const char* pattern, const char* moduleName);

// Safe memory reading
template<typename T>
T Read(uintptr_t addr) {
    __try {
        return *reinterpret_cast<T*>(addr);
    } __except(1) {
        return T{};
    }
}

template<typename T>
T ReadChain(uintptr_t base, std::initializer_list<ptrdiff_t> offsets) {
    uintptr_t addr = base;
    auto it = offsets.begin();
    // Follow all but last offset as pointer dereferences
    for (size_t i = 0; i < offsets.size() - 1; ++i, ++it) {
        addr = Read<uintptr_t>(addr + *it);
        if (addr == 0) return T{};
    }
    return Read<T>(addr + *it);
}

} // namespace Memory
