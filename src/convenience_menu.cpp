#include "convenience_menu.h"
#include "coop_resync.h"
#include "memory.h"
#include "game_textures.h"
#include <Windows.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace ConvenienceMenu {

// The game's own "give item" routine (ItemGib in the debug tools / cheat tables):
//   ItemGib(MapItemMan*, ItemList*, header, 0) with ItemList = header + 0x20:
//   u32 count, then per item { u32 id | category, u32 quantity, u32 0, s32 ash of war (-1) }.
// Signatures from Nordgaren's Erd-Tools (Offsets.cs), checked against the 2.7.2 exe.
static const char* ITEM_GIVE_AOB = "8B 02 83 F8 0A";
static constexpr ptrdiff_t ITEM_GIVE_OFFSET = -0x52;
static const char* MAP_ITEM_MAN_AOB = "48 8B 0D ?? ?? ?? ?? C7 44 24 50 FF FF FF FF C7 45 A0 FF FF FF FF 48 85 C9 75 2E";

using ItemGibFn = void(*)(void* mapItemMan, void* itemList, void* header, uint64_t zero);

struct Entry {
    std::string section;
    std::string label;
    uint32_t id = 0;        // with the category bits
    int quantity = 1;
};

static std::vector<Entry> s_Entries;
static std::string s_FileError;
static std::string s_Status;
static double s_StatusUntil = 0.0;
static bool s_WasOpen = false;

static ItemGibFn s_ItemGib = nullptr;
static uintptr_t s_MapItemManStatic = 0;
static bool s_Resolved = false;

static bool Resolve() {
    if (s_Resolved) return s_ItemGib && s_MapItemManStatic;
    s_Resolved = true;
    uintptr_t give = Memory::AOBScan(ITEM_GIVE_AOB, "eldenring.exe");
    uintptr_t man = Memory::AOBScan(MAP_ITEM_MAN_AOB, "eldenring.exe");
    if (give) {
        uintptr_t fn = give + ITEM_GIVE_OFFSET;
        // expect the function's prologue: push rbp; push rsi; push rdi
        if (memcmp(reinterpret_cast<void*>(fn), "\x40\x55\x56\x57", 4) == 0) s_ItemGib = reinterpret_cast<ItemGibFn>(fn);
        else Log("ConvenienceMenu: ItemGib prologue mismatch at %llX", (unsigned long long)fn);
    }
    if (man) s_MapItemManStatic = man + 7 + *reinterpret_cast<int32_t*>(man + 3);
    Log("ConvenienceMenu: ItemGib=%p MapItemMan static=%llX", (void*)s_ItemGib, (unsigned long long)s_MapItemManStatic);
    return s_ItemGib && s_MapItemManStatic;
}

static uint32_t CategoryBits(const char* cat, bool& ok) {
    ok = true;
    if (!_stricmp(cat, "weapon")) return 0x00000000;
    if (!_stricmp(cat, "armor") || !_stricmp(cat, "protector")) return 0x10000000;
    if (!_stricmp(cat, "talisman") || !_stricmp(cat, "accessory")) return 0x20000000;
    if (!_stricmp(cat, "goods") || !_stricmp(cat, "item")) return 0x40000000;
    if (!_stricmp(cat, "ash") || !_stricmp(cat, "gem")) return 0x80000000;
    ok = false;
    return 0;
}

// ConvenienceMenu.txt:  [Section]  /  <category> <id> [xQuantity] <label...>  /  # comment
static void LoadFile() {
    s_Entries.clear();
    s_FileError.clear();
    char path[MAX_PATH];
    GameTextures::PathNextToDll("ConvenienceMenu.txt", path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) { s_FileError = "ConvenienceMenu.txt not found next to the DLL"; return; }

    std::string section;
    char line[512];
    int lineNo = 0;
    while (fgets(line, sizeof(line), f)) {
        lineNo++;
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            section.assign(s + 1, end ? end - s - 1 : strlen(s + 1));
            continue;
        }
        char cat[32] = {};
        long id = 0;
        int used = 0;
        if (sscanf(s, "%31s %ld %n", cat, &id, &used) < 2) {
            Log("ConvenienceMenu: line %d not understood: %s", lineNo, s);
            continue;
        }
        bool ok;
        Entry e;
        e.section = section;
        e.id = CategoryBits(cat, ok) | (uint32_t)id;
        if (!ok) { Log("ConvenienceMenu: line %d: unknown category '%s'", lineNo, cat); continue; }
        const char* rest = s + used;
        if (*rest == 'x' || *rest == 'X') {
            e.quantity = atoi(rest + 1);
            while (*rest && *rest != ' ' && *rest != '\t') rest++;
            while (*rest == ' ' || *rest == '\t') rest++;
        }
        if (e.quantity < 1) e.quantity = 1;
        e.label = *rest ? rest : std::to_string(id);
        s_Entries.push_back(e);
    }
    fclose(f);
    Log("ConvenienceMenu: %zu entries from %s", s_Entries.size(), path);
}

struct GiveRequest { uint32_t id; int quantity; };

static bool CallItemGib(void* mapItemMan, uint8_t* buf) {
    __try {
        s_ItemGib(mapItemMan, buf + 0x20, buf, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Runs on its own short-lived thread, like the debug tools' injected call
static DWORD WINAPI GiveThread(LPVOID param) {
    GiveRequest req = *static_cast<GiveRequest*>(param);
    delete static_cast<GiveRequest*>(param);
    void* mapItemMan = Memory::Read<void*>(s_MapItemManStatic);
    if (!mapItemMan) { Log("ConvenienceMenu: MapItemMan not ready"); return 1; }

    alignas(16) uint8_t buf[0x40] = {};
    int32_t ash = -1;
    uint32_t count = 1, quantity = (uint32_t)req.quantity;
    memcpy(buf + 0x20, &count, 4);
    memcpy(buf + 0x24, &req.id, 4);
    memcpy(buf + 0x28, &quantity, 4);
    memcpy(buf + 0x30, &ash, 4);
    bool ok = CallItemGib(mapItemMan, buf);
    Log("ConvenienceMenu: gave %08X x%d %s", req.id, req.quantity, ok ? "ok" : "FAILED (exception)");
    return ok ? 0 : 1;
}

bool GiveItem(uint32_t id, int quantity) {
    if (!Memory::GetChrIns() || !Resolve()) return false;
    auto* req = new GiveRequest{ id, quantity };
    HANDLE th = CreateThread(nullptr, 0, GiveThread, req, 0, nullptr);
    if (!th) {
        delete req;
        return false;
    }
    CloseHandle(th);
    return true;
}

static void Give(const Entry& e) {
    char msg[256];
    if (!Memory::GetChrIns()) {
        snprintf(msg, sizeof(msg), "Load into the game first");
    } else if (!Resolve()) {
        snprintf(msg, sizeof(msg), "Couldn't find the game's item function (see the log)");
    } else {
        auto* req = new GiveRequest{ e.id, e.quantity };
        HANDLE th = CreateThread(nullptr, 0, GiveThread, req, 0, nullptr);
        if (th) {
            CloseHandle(th);
            if (e.quantity > 1) snprintf(msg, sizeof(msg), "Received %s x%d", e.label.c_str(), e.quantity);
            else snprintf(msg, sizeof(msg), "Received %s", e.label.c_str());
        } else {
            delete req;
            snprintf(msg, sizeof(msg), "Couldn't start the give thread");
        }
    }
    s_Status = msg;
    s_StatusUntil = ImGui::GetTime() + 4.0;
}

void Draw(ImFont* font, bool open) {
    if (open && !s_WasOpen) LoadFile();
    s_WasOpen = open;
    if (!open) return;

    auto& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    if (font) ImGui::PushFont(font, floorf(22.0f * scale));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, io.DisplaySize.y * 0.8f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 14, 11, 235));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(146, 139, 108, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(30, 27, 21, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(44, 39, 30, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(226, 214, 176, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(58, 52, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(92, 84, 62, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(120, 110, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(92, 84, 70, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));

    ImGui::Begin("Convenience  (F8 to close)", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    CoopResync::DrawMenuSection(scale);

    if (!s_FileError.empty()) {
        ImGui::TextUnformatted(s_FileError.c_str());
    } else if (s_Entries.empty()) {
        ImGui::TextUnformatted("ConvenienceMenu.txt has no entries");
    }

    std::string section = "\x01";
    for (size_t i = 0; i < s_Entries.size(); i++) {
        const Entry& e = s_Entries[i];
        if (e.section != section) {
            section = e.section;
            if (!section.empty()) {
                if (i) ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.71f, 0.68f, 0.52f, 1.0f), "%s", section.c_str());
                ImGui::Separator();
            }
        }
        ImGui::PushID((int)i);
        if (e.quantity > 1) ImGui::Text("%s  x%d", e.label.c_str(), e.quantity);
        else ImGui::TextUnformatted(e.label.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 80.0f * scale);
        if (ImGui::Button("Receive", ImVec2(80.0f * scale, 0))) Give(e);
        ImGui::PopID();
    }

    if (!s_Status.empty() && ImGui::GetTime() < s_StatusUntil) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted(s_Status.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(9);
    if (font) ImGui::PopFont();
}

} // namespace ConvenienceMenu
