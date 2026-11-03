#include "item_icons.h"
#include "d3dhook.h"
#include "game_textures.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

extern void Log(const char* fmt, ...);

namespace ItemIcons {

using OodleDecompressFn = int64_t(WINAPI*)(const void* comp, int64_t compSize, void* raw, int64_t rawSize,
    int fuzzSafe, int checkCrc, int verbosity, void* decBufBase, int64_t decBufSize,
    void* callback, void* callbackData, void* decoderMemory, int64_t decoderMemorySize, int threadPhase);

constexpr int kCell = 128, kAtlas = 2048, kPerRow = kAtlas / kCell, kSlots = kPerRow * kPerRow;
constexpr UINT RGBA = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM

// ---- the reading thread ----

struct File {
    uint32_t offset, size;  // in 00_Solo.tpfbdt
};
static std::unordered_map<int, File> s_Files;  // icon id -> its .tpf.dcx (the thread's)

static std::mutex s_Lock;
static std::condition_variable s_Wake;
static std::unordered_map<int, uint64_t> s_Wanted;  // under s_Lock: icons to read -> the frame they were last drawn
static std::unordered_set<int> s_Delivered;         // under s_Lock: read and handed over (until the atlas drops them)
static std::deque<std::pair<int, std::vector<uint8_t>>> s_Done;  // under s_Lock: read, for the render thread (empty: none)
static std::atomic<int> s_State{ 0 };      // 0 not started, 1 running, -1 no icons
static std::atomic<uint64_t> s_Frame{ 0 };

// 00_Solo.tpfbhd (BHF4): file headers of 0x24 bytes from 0x40 {flags, -1, s64 size, s64 size, u32 offset, id,
// name offset}, names in UTF-16 ("00_Solo\MENU_Knowledge_00001.tpf.dcx")
static bool ReadIndex(const char* bhd) {
    FILE* f = fopen(bhd, "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.resize(size > 0 ? size : 0);
    bool ok = size > 0x40 && fread(b.data(), 1, size, f) == (size_t)size;
    fclose(f);
    if (!ok || memcmp(b.data(), "BHF4", 4)) return false;
    uint32_t count;
    uint64_t headerSize;
    memcpy(&count, &b[0xc], 4);
    memcpy(&headerSize, &b[0x20], 8);
    if (headerSize != 0x24 || 0x40 + (uint64_t)count * 0x24 > b.size()) return false;
    static const wchar_t kPrefix[] = L"MENU_Knowledge_";
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* h = &b[0x40 + i * 0x24];
        uint64_t compSize;
        uint32_t offset, nameAt;
        memcpy(&compSize, h + 8, 8);
        memcpy(&offset, h + 0x18, 4);
        memcpy(&nameAt, h + 0x20, 4);
        std::wstring name;
        for (size_t at = nameAt; at + 1 < b.size(); at += 2) {
            wchar_t c = (wchar_t)(b[at] | b[at + 1] << 8);
            if (!c) break;
            name += c;
        }
        size_t p = name.find(kPrefix);
        if (p == std::wstring::npos || compSize > (64u << 20)) continue;
        s_Files[_wtoi(name.c_str() + p + wcslen(kPrefix))] = { offset, (uint32_t)compSize };
    }
    return !s_Files.empty();
}

// DCX (KRAK) -> TPF -> its one DDS
static bool Unpack(OodleDecompressFn decompress, const std::vector<uint8_t>& dcx, std::vector<uint8_t>& dds) {
    if (dcx.size() < 0x4c || memcmp(dcx.data(), "DCX\0", 4) || memcmp(dcx.data() + 0x28, "KRAK", 4)) return false;
    const uint8_t* s = dcx.data();
    uint32_t raw = (uint32_t)s[0x1c] << 24 | s[0x1d] << 16 | s[0x1e] << 8 | s[0x1f];
    uint32_t comp = (uint32_t)s[0x20] << 24 | s[0x21] << 16 | s[0x22] << 8 | s[0x23];
    if (comp > dcx.size() - 0x4c || raw > (1u << 26)) return false;
    std::vector<uint8_t> tpf(raw + 64);
    if (decompress(s + 0x4c, comp, tpf.data(), raw, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3) != raw) return false;
    if (memcmp(tpf.data(), "TPF\0", 4)) return false;
    uint32_t off, size;
    memcpy(&off, tpf.data() + 0x10, 4);
    memcpy(&size, tpf.data() + 0x14, 4);
    if ((uint64_t)off + size > raw || size < 0x94 || memcmp(tpf.data() + off, "DDS ", 4)) return false;
    dds.assign(tpf.begin() + off, tpf.begin() + off + size);
    return true;
}

// A BC1/BC3/BC7 DDS, decoded and box-filtered (with its alpha) down to kCell square, as an RGBA DDS for the atlas
static bool Shrink(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    uint32_t h, w, dxgi = 0;
    memcpy(&h, &in[0x0c], 4);
    memcpy(&w, &in[0x10], 4);
    size_t data = 0x80;
    if (!memcmp(&in[0x54], "DX10", 4)) {
        memcpy(&dxgi, &in[0x80], 4);
        data = 0x94;
    } else if (!memcmp(&in[0x54], "DXT1", 4)) dxgi = 71;
    else if (!memcmp(&in[0x54], "DXT5", 4)) dxgi = 77;
    int kind = dxgi == 71 || dxgi == 72 ? 1 : dxgi == 77 || dxgi == 78 ? 3 : dxgi == 98 || dxgi == 99 ? 7 : 0;
    if (!kind || w < 4 || h < 4 || w > 4096 || h > 4096 || (w | h) & 3) return false;
    size_t blockBytes = kind == 1 ? 8 : 16;
    if (data + (size_t)(w / 4) * (h / 4) * blockBytes > in.size()) return false;
    std::vector<uint8_t> px((size_t)w * h * 4);
    const uint8_t* src = in.data() + data;
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++, src += blockBytes) {
            uint8_t* dst = px.data() + ((size_t)by * 4 * w + bx * 4) * 4;
            if (kind == 1) bcdec_bc1(src, dst, w * 4);
            else if (kind == 3) bcdec_bc3(src, dst, w * 4);
            else bcdec_bc7(src, dst, w * 4);
        }
    out.assign(0x94 + kCell * kCell * 4, 0);
    memcpy(out.data(), "DDS ", 4);
    uint32_t cell = kCell, format = RGBA;
    memcpy(&out[0x0c], &cell, 4);
    memcpy(&out[0x10], &cell, 4);
    memcpy(&out[0x54], "DX10", 4);
    memcpy(&out[0x80], &format, 4);
    uint8_t* o = out.data() + 0x94;
    for (int y = 0; y < kCell; y++)
        for (int x = 0; x < kCell; x++, o += 4) {
            uint32_t x0 = x * w / kCell, x1 = (std::max)(x0 + 1, (x + 1) * w / kCell);
            uint32_t y0 = y * h / kCell, y1 = (std::max)(y0 + 1, (y + 1) * h / kCell);
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (uint32_t sy = y0; sy < y1; sy++)
                for (uint32_t sx = x0; sx < x1; sx++, n++) {
                    const uint8_t* p = &px[((size_t)sy * w + sx) * 4];
                    r += p[0] * p[3], g += p[1] * p[3], b += p[2] * p[3], a += p[3];
                }
            if (!a) continue;
            o[0] = (uint8_t)(r / a), o[1] = (uint8_t)(g / a), o[2] = (uint8_t)(b / a), o[3] = (uint8_t)(a / n);
        }
    return true;
}

static void Thread() {
    char bhd[MAX_PATH];
    GameTextures::PathNextToDll("..\\elden-vins\\menu\\hi\\00_Solo.tpfbhd", bhd, sizeof(bhd));
    std::string bdt = bhd;
    bdt.replace(bdt.size() - 3, 3, "bdt");
    HANDLE file = CreateFileA(bdt.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
    auto decompress = oodle ? reinterpret_cast<OodleDecompressFn>(GetProcAddress(oodle, "OodleLZ_Decompress")) : nullptr;
    if (!ReadIndex(bhd) || file == INVALID_HANDLE_VALUE || !decompress) {
        Log("ItemIcons: can't read %s or load Oodle: no item icons", bhd);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        s_State = -1;
        return;
    }
    Log("ItemIcons: %zu icons in %s", s_Files.size(), bhd);
    int failures = 0;
    for (;;) {
        int icon = -1;
        {
            std::unique_lock<std::mutex> lock(s_Lock);
            s_Wake.wait_for(lock, std::chrono::milliseconds(500), [] { return !s_Wanted.empty(); });
            // the one drawn most recently; forget the ones not drawn for a while (scrolled away)
            uint64_t now = s_Frame.load(), best = 0;
            for (auto it = s_Wanted.begin(); it != s_Wanted.end();) {
                if (now - it->second > 30) {
                    it = s_Wanted.erase(it);
                    continue;
                }
                if (icon < 0 || it->second > best) icon = it->first, best = it->second;
                ++it;
            }
            if (icon >= 0) s_Wanted.erase(icon);
        }
        if (icon < 0) continue;
        std::vector<uint8_t> out;
        auto f = s_Files.find(icon);
        if (f != s_Files.end()) {
            std::vector<uint8_t> dcx(f->second.size), dds;
            LARGE_INTEGER at;
            at.QuadPart = f->second.offset;
            DWORD got = 0;
            bool ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, dcx.data(), f->second.size, &got, nullptr) &&
                      got == f->second.size && Unpack(decompress, dcx, dds) && Shrink(dds, out);
            if (!ok) {
                out.clear();
                if (failures++ < 5) Log("ItemIcons: icon %d didn't decode", icon);
            }
        }
        std::lock_guard<std::mutex> lock(s_Lock);
        s_Delivered.insert(icon);
        s_Done.emplace_back(icon, std::move(out));
    }
}

// ---- the atlas (render thread) ----

static uint64_t s_Atlas = 0;
static bool s_AtlasFailed = false;
static std::unordered_map<int, int> s_Slot;  // icon id -> atlas slot
static std::unordered_set<int> s_None;       // icons that didn't read: not asked for again
static std::vector<int> s_SlotIcon(kSlots, -1);
static std::vector<uint64_t> s_SlotDrawn(kSlots, 0);

bool Draw(ImDrawList* dl, int iconId, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (iconId <= 0 || s_State.load() < 0 || s_AtlasFailed) return false;
    auto it = s_Slot.find(iconId);
    if (it != s_Slot.end()) {
        int slot = it->second;
        s_SlotDrawn[slot] = s_Frame.load();
        float u = (float)(slot % kPerRow) / kPerRow, v = (float)(slot / kPerRow) / kPerRow, d = 1.0f / kPerRow;
        dl->AddImage(ImTextureRef((ImTextureID)s_Atlas), a, b, ImVec2(u, v), ImVec2(u + d, v + d), tint);
        return true;
    }
    if (s_None.count(iconId)) return false;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        if (!s_Delivered.count(iconId)) s_Wanted[iconId] = s_Frame.load();
    }
    s_Wake.notify_one();
    int idle = 0;
    if (s_State.compare_exchange_strong(idle, 1)) std::thread(Thread).detach();
    return false;
}

void Update() {
    s_Frame++;
    std::vector<std::pair<int, std::vector<uint8_t>>> got;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        while (!s_Done.empty() && got.size() < 8) {
            got.push_back(std::move(s_Done.front()));
            s_Done.pop_front();
        }
    }
    if (got.empty()) return;
    if (!s_Atlas && !s_AtlasFailed) {
        s_Atlas = D3DHook::CreateAtlas(kAtlas, kAtlas, RGBA);
        s_AtlasFailed = !s_Atlas;
        if (s_AtlasFailed) Log("ItemIcons: no atlas: no item icons");
    }
    if (!s_Atlas) return;
    std::vector<D3DHook::AtlasRegion> regions;
    for (auto& [icon, dds] : got) {
        if (dds.empty()) {
            s_None.insert(icon);
            continue;
        }
        // a free slot, else the one drawn longest ago
        int slot = 0;
        for (int i = 0; i < kSlots; i++) {
            if (s_SlotIcon[i] < 0) { slot = i; break; }
            if (s_SlotDrawn[i] < s_SlotDrawn[slot]) slot = i;
        }
        if (s_SlotIcon[slot] >= 0) {
            s_Slot.erase(s_SlotIcon[slot]);
            std::lock_guard<std::mutex> lock(s_Lock);
            s_Delivered.erase(s_SlotIcon[slot]);
        }
        s_SlotIcon[slot] = icon;
        s_SlotDrawn[slot] = s_Frame.load();
        regions.push_back({ (UINT)(slot % kPerRow * kCell), (UINT)(slot / kPerRow * kCell), dds.data(), dds.size() });
        s_Slot[icon] = slot;
    }
    if (!regions.empty() && D3DHook::UpdateAtlasMany(s_Atlas, regions.data(), (int)regions.size()) != (int)regions.size())
        Log("ItemIcons: %d icons didn't upload", (int)regions.size());
}

} // namespace ItemIcons
