#include "hotkey_sheet.h"
#include "game_textures.h"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace HotkeySheet {

struct Line {
    bool section = false;
    std::string key;    // or the section title
    std::string text;
};

static std::vector<Line> s_Lines;
static bool s_WasOpen = false;

static std::string Trim(const char* b, const char* e) {
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    return std::string(b, e);
}

// Hotkeys.txt:  [Section]  /  <key> | <what it does>  /  # comment
static void LoadFile() {
    s_Lines.clear();
    char path[MAX_PATH];
    GameTextures::PathNextToDll("Hotkeys.txt", path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        s_Lines.push_back({ false, "", "Hotkeys.txt not found next to the DLL" });
        return;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = Trim(buf, buf + strlen(buf));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            s_Lines.push_back({ true, line.substr(1, end == std::string::npos ? std::string::npos : end - 1), "" });
            continue;
        }
        size_t bar = line.find('|');
        if (bar == std::string::npos) s_Lines.push_back({ false, "", line });
        else s_Lines.push_back({ false, Trim(line.c_str(), line.c_str() + bar), Trim(line.c_str() + bar + 1, line.c_str() + line.size()) });
    }
    fclose(f);
    Log("HotkeySheet: %zu lines from %s", s_Lines.size(), path);
}

void Draw(ImFont* font, bool open) {
    if (open && !s_WasOpen) LoadFile();
    s_WasOpen = open;
    if (!open) return;

    auto& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    if (font) ImGui::PushFont(font, floorf(19.0f * scale));

    float margin = floorf(24.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 14, 11, 215));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(146, 139, 108, 200));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(226, 214, 176, 255));
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(92, 84, 70, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 3.0f * scale));
    ImGui::Begin("##HotkeySheet", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    // key column as wide as the widest key
    float keyWidth = 0.0f;
    for (const Line& l : s_Lines)
        if (!l.section && !l.key.empty()) keyWidth = fmaxf(keyWidth, ImGui::CalcTextSize(l.key.c_str()).x);

    const ImVec4 gold(0.71f, 0.68f, 0.52f, 1.0f);
    for (size_t i = 0; i < s_Lines.size(); i++) {
        const Line& l = s_Lines[i];
        if (l.section) {
            if (i) ImGui::Spacing();
            ImGui::TextColored(gold, "%s", l.key.c_str());
            ImGui::Separator();
        } else if (l.key.empty()) {
            ImGui::TextUnformatted(l.text.c_str());
        } else {
            ImGui::TextColored(gold, "%s", l.key.c_str());
            ImGui::SameLine(keyWidth + ImGui::GetStyle().WindowPadding.x + 16.0f * scale);
            ImGui::TextUnformatted(l.text.c_str());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    if (font) ImGui::PopFont();
}

} // namespace HotkeySheet
