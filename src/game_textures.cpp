#include "game_textures.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

extern void Log(const char* fmt, ...);

namespace GameTextures {

// OodleLZ_Decompress from oo2core_6_win64.dll (next to the game exe)
using OodleDecompressFn = int64_t(WINAPI*)(const void* comp, int64_t compSize, void* raw, int64_t rawSize,
    int fuzzSafe, int checkCrc, int verbosity, void* decBufBase, int64_t decBufSize,
    void* callback, void* callbackData, void* decoderMemory, int64_t decoderMemorySize, int threadPhase);

static uint32_t ReadBE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t ReadLE32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

void PathNextToDll(const char* relative, char* out, size_t outSize) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&PathNextToDll, &self);
    char dll[MAX_PATH] = {};
    GetModuleFileNameA(self, dll, MAX_PATH);
    char* slash = strrchr(dll, '\\');
    if (slash) slash[1] = 0;
    snprintf(out, outSize, "%s%s", dll, relative);
}

static bool ReadWholeFile(const char* path, std::vector<uint8_t>& data) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(size > 0 ? size : 0);
    bool ok = size > 0 && fread(data.data(), 1, size, f) == (size_t)size;
    fclose(f);
    return ok;
}

bool LoadDds(const char* tpfDcxPath, const wchar_t* name, std::vector<uint8_t>& dds) {
    std::vector<uint8_t> dcx;
    if (!ReadWholeFile(tpfDcxPath, dcx)) { Log("GameTextures: can't read %s", tpfDcxPath); return false; }

    // DCX: "DCX\0" ... "DCS\0" raw/compressed sizes (big endian) at 0x1C/0x20, "KRAK" at 0x28, data at 0x4C
    if (dcx.size() < 0x4C || memcmp(dcx.data(), "DCX\0", 4) != 0 || memcmp(dcx.data() + 0x28, "KRAK", 4) != 0) {
        Log("GameTextures: %s is not a Kraken DCX", tpfDcxPath);
        return false;
    }
    uint32_t rawSize = ReadBE32(dcx.data() + 0x1C);
    uint32_t compSize = ReadBE32(dcx.data() + 0x20);
    if (0x4C + (size_t)compSize > dcx.size()) { Log("GameTextures: truncated DCX"); return false; }

    HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
    auto decompress = oodle ? (OodleDecompressFn)GetProcAddress(oodle, "OodleLZ_Decompress") : nullptr;
    if (!decompress) { Log("GameTextures: oo2core_6_win64.dll not available"); return false; }

    std::vector<uint8_t> tpf(rawSize);
    int64_t got = decompress(dcx.data() + 0x4C, compSize, tpf.data(), rawSize,
        1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    dcx.clear(); dcx.shrink_to_fit();
    if (got != rawSize) { Log("GameTextures: decompress returned %lld of %u", (long long)got, rawSize); return false; }

    // TPF (PC): "TPF\0", data size, file count, platform, flag2, encoding (1 = UTF-16 names), 0;
    // then per texture: data offset, size, format, type, mipmaps, flags1, name offset, hasFloatStruct
    if (rawSize < 0x10 || memcmp(tpf.data(), "TPF\0", 4) != 0) { Log("GameTextures: not a TPF"); return false; }
    uint32_t count = ReadLE32(&tpf[8]);
    uint8_t encoding = tpf[0xE];
    size_t off = 0x10;
    for (uint32_t i = 0; i < count && off + 0x14 <= rawSize; i++) {
        uint32_t dataOff = ReadLE32(&tpf[off]);
        uint32_t dataSize = ReadLE32(&tpf[off + 4]);
        uint8_t flags1 = tpf[off + 11];
        uint32_t nameOff = ReadLE32(&tpf[off + 12]);
        uint32_t hasFloat = ReadLE32(&tpf[off + 16]);
        off += 0x14;
        if (hasFloat && off + 8 <= rawSize) off += 8 + ReadLE32(&tpf[off + 4]);

        if (encoding != 1 || nameOff >= rawSize) continue;
        size_t len = wcslen(name);
        if (nameOff + (len + 1) * 2 > rawSize) continue;
        if (memcmp(&tpf[nameOff], name, len * 2) != 0 || tpf[nameOff + len * 2] || tpf[nameOff + len * 2 + 1]) continue;

        if (flags1 == 2 || flags1 == 3) { Log("GameTextures: %ls is compressed inside the TPF", name); return false; }
        if ((size_t)dataOff + dataSize > rawSize) { Log("GameTextures: bad offset for %ls", name); return false; }
        dds.assign(tpf.begin() + dataOff, tpf.begin() + dataOff + dataSize);
        Log("GameTextures: loaded %ls (%u bytes) from %s", name, dataSize, tpfDcxPath);
        return true;
    }
    Log("GameTextures: %ls not found in %s", name, tpfDcxPath);
    return false;
}

} // namespace GameTextures
