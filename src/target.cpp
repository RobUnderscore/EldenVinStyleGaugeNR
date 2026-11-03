#include "target.h"
#include "minimap.h"
#include "world.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace Target {

using namespace World;

static constexpr uintptr_t WORLD_CHR_MAN = 0x3d6d018;
static constexpr uintptr_t CHR_SETS = 0x1ded8;  // WorldChrMan: ChrSet pointers; a set: entries at +0x18, capacity at +0x10

struct Effect {
    int32_t icon;
    float remaining;
};
struct View {
    bool on = false;
    std::string name;
    float hp = 0, hpMax = 0, posture = 0, postureMax = 0;
    float buildup[7] = {}, buildupMax[7] = {};
    std::vector<Effect> effects;
};
static std::mutex s_Lock;
static View s_View;
static Table s_Npc;

// The character with this FieldInsHandle (ChrIns+0x8): its set by the handle's selector, else every set
static uintptr_t ChrByHandle(uint64_t handle) {
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    if (!wcm) return 0;
    int first = (int)((handle >> 20) & 0xff);
    for (int pass = 0; pass < 2; pass++) {
        for (int s = pass ? 0 : first; s < (pass ? 196 : first + 1); s++) {
            uintptr_t set = Ptr(wcm + CHR_SETS + s * 8);
            uintptr_t entries = set ? Ptr(set + 0x18) : 0;
            uint32_t cap = set ? Rd<uint32_t>(set + 0x10) : 0;
            if (!entries || cap > 0x4000) continue;
            for (uint32_t i = 0; i < cap; i++) {
                uintptr_t chr = Ptr(entries + i * 16);
                if (chr && Rd<uint64_t>(chr + 8) == handle) return chr;
            }
        }
    }
    return 0;
}

// The resist module keeps ints; read a float instead if one turns up
static float Num(uintptr_t a) {
    uint32_t raw = Rd<uint32_t>(a);
    if (raw <= 1000000) return (float)raw;
    float f;
    memcpy(&f, &raw, 4);
    return std::isfinite(f) ? f : 0.0f;
}

void RunFrame() {
    View v;
    uintptr_t main = Base() && Ready() ? MainPlayer() : 0;
    uint64_t handle = main ? Rd<uint64_t>(main + 0x6b0) : ~0ull;
    uintptr_t chr = handle != ~0ull && (uint32_t)handle != 0xffffffffu ? ChrByHandle(handle) : 0;
    if (chr && chr != main) {
        uintptr_t mods = Ptr(chr + 0x190);
        uintptr_t data = mods ? Ptr(mods) : 0, resist = mods ? Ptr(mods + 0x20) : 0, armor = mods ? Ptr(mods + 0x40) : 0;
        if (data) {
            v.on = true;
            v.hp = (float)Rd<int32_t>(data + 0x138), v.hpMax = (float)Rd<int32_t>(data + 0x13c);
            if (armor) v.posture = Rd<float>(armor + 0x10), v.postureMax = Rd<float>(armor + 0x14);
            if (resist)
                for (int i = 0; i < 7; i++) {
                    float cur = Num(resist + 0x10 + i * 4), max = Num(resist + 0x2c + i * 4);
                    if (max >= 1.0f && max <= 998.0f && cur <= max) v.buildup[i] = max - cur, v.buildupMax[i] = max;
                }
            if (!s_Npc.rows) s_Npc = FindTable(L"NpcParam");
            uintptr_t row = s_Npc.rows ? s_Npc.Row(Rd<int32_t>(chr + 0x60)) : 0;
            v.name = row ? Text(NPC_NAME, Rd<int32_t>(row + 0x0c)) : std::string();
            // its effects, one per icon (the longest)
            uintptr_t ctrl = Ptr(chr + 0x178);
            uintptr_t node = ctrl ? Ptr(ctrl + 8) : 0;
            for (int n = 0; node && n < 200; n++) {
                uintptr_t prow = Ptr(node);
                int32_t icon = prow ? Rd<int32_t>(prow) : -1;
                float left = Rd<float>(node + 0x40);
                if (icon > 0 && left > 0.0f && left < 100000.0f) {
                    auto it = std::find_if(v.effects.begin(), v.effects.end(), [&](const Effect& e) { return e.icon == icon; });
                    if (it == v.effects.end()) v.effects.push_back({ icon, left });
                    else it->remaining = (std::max)(it->remaining, left);
                }
                uintptr_t next = Ptr(node + 0x30);
                if (next == node) break;
                node = next;
            }
            if (v.effects.size() > 12) v.effects.resize(12);
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_View = std::move(v);
}

static const char* const kBuildupNames[7] = { "Poison", "Scarlet Rot", "Bleed", "Death Blight", "Frost", "Sleep", "Madness" };
static const ImU32 kBuildupColors[7] = {
    IM_COL32(110, 170, 70, 255), IM_COL32(210, 90, 50, 255), IM_COL32(190, 30, 40, 255), IM_COL32(150, 140, 160, 255),
    IM_COL32(150, 210, 240, 255), IM_COL32(130, 120, 220, 255), IM_COL32(240, 190, 60, 255),
};

static void Bar(ImDrawList* dl, ImFont* f, float fs, ImVec2 at, float w, float h, float frac, ImU32 col, const char* left, const char* right) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(20, 18, 14, 200));
    dl->AddRectFilled(at, ImVec2(at.x + w * frac, at.y + h), col);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), IM_COL32(146, 139, 108, 200));
    float ty = at.y + (h - fs) * 0.5f;
    if (left && *left) {
        dl->AddText(f, fs, ImVec2(at.x + 5, ty + 1), IM_COL32(0, 0, 0, 220), left);
        dl->AddText(f, fs, ImVec2(at.x + 4, ty), IM_COL32(240, 232, 210, 255), left);
    }
    if (right && *right) {
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, right);
        dl->AddText(f, fs, ImVec2(at.x + w - ts.x - 3, ty + 1), IM_COL32(0, 0, 0, 220), right);
        dl->AddText(f, fs, ImVec2(at.x + w - ts.x - 4, ty), IM_COL32(240, 232, 210, 255), right);
    }
}

void Draw(ImFont* font, const BuffTimers::Sheets& sheets) {
    View v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        v = s_View;
    }
    if (!v.on) return;
    auto& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    ImFont* f = font ? font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // under the minimap (radius 125, 47 px from the top, 48 from the right at 1080p), or in its place when it's off
    float w = 250.0f * scale, x = io.DisplaySize.x - 48.0f * scale - w, y = (Minimap::Enabled() ? 47.0f + 250.0f + 14.0f : 47.0f) * scale;
    float fs = floorf(15.0f * scale), sfs = floorf(13.0f * scale), h = 16.0f * scale, gap = 4.0f * scale;
    if (!v.name.empty()) {
        dl->AddText(f, fs, ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 220), v.name.c_str());
        dl->AddText(f, fs, ImVec2(x, y), IM_COL32(230, 214, 170, 255), v.name.c_str());
        y += fs + gap;
    }
    char a[48];
    snprintf(a, sizeof(a), "%.0f / %.0f", (std::max)(v.hp, 0.0f), v.hpMax);
    Bar(dl, f, sfs, ImVec2(x, y), w, h, v.hpMax > 0 ? v.hp / v.hpMax : 0, IM_COL32(150, 30, 30, 235), "HP", a);
    y += h + gap;
    if (v.postureMax > 0) {
        snprintf(a, sizeof(a), "%.0f / %.0f", (std::max)(v.posture, 0.0f), v.postureMax);
        Bar(dl, f, sfs, ImVec2(x, y), w, h * 0.8f, v.posture / v.postureMax, IM_COL32(200, 160, 60, 235), "Posture", a);
        y += h * 0.8f + gap;
    }
    for (int i = 0; i < 7; i++) {
        if (v.buildup[i] <= 0.0f) continue;
        snprintf(a, sizeof(a), "%.0f / %.0f", v.buildup[i], v.buildupMax[i]);
        Bar(dl, f, sfs, ImVec2(x, y), w, h * 0.75f, v.buildup[i] / v.buildupMax[i], kBuildupColors[i], kBuildupNames[i], a);
        y += h * 0.75f + gap;
    }
    // effects with the time left, under the bars
    float icon = 28.0f * scale, ix = x;
    for (const Effect& e : v.effects) {
        int sheet;
        float sx, sy, sw, sh;
        if (!BuffTimers::IconRect(e.icon, sheet, sx, sy, sw, sh) || !sheets.tex[sheet]) continue;
        if (ix + icon > x + w + 1) {
            ix = x;
            y += icon + sfs + gap;
        }
        ImVec2 uv0(sx / sheets.w[sheet], sy / sheets.h[sheet]), uv1((sx + sw) / sheets.w[sheet], (sy + sh) / sheets.h[sheet]);
        dl->AddImage(ImTextureRef(sheets.tex[sheet]), ImVec2(ix, y), ImVec2(ix + icon, y + icon), uv0, uv1);
        char t[16];
        if (e.remaining >= 60.0f) snprintf(t, sizeof(t), "%d:%02d", (int)e.remaining / 60, (int)e.remaining % 60);
        else snprintf(t, sizeof(t), "%ds", (int)ceilf(e.remaining));
        ImVec2 ts = f->CalcTextSizeA(sfs, FLT_MAX, 0.0f, t);
        dl->AddText(f, sfs, ImVec2(ix + (icon - ts.x) * 0.5f + 1, y + icon + 1), IM_COL32(0, 0, 0, 220), t);
        dl->AddText(f, sfs, ImVec2(ix + (icon - ts.x) * 0.5f, y + icon), IM_COL32(226, 214, 176, 255), t);
        ix += icon + gap;
    }
}

} // namespace Target
