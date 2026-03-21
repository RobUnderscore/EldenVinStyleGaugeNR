#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Memory {

// Initialize - find WorldChrMan via AOB scan
bool Init();

// Get base addresses (returns 0 if not found)
uintptr_t GetWorldChrMan();
uintptr_t GetChrIns();
uintptr_t GetSpEffectCtrl();
uintptr_t GetSpEffectListHead();

// AOB scan helper
uintptr_t AOBScan(const char* pattern, const char* moduleName = "nightreign.exe");

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
