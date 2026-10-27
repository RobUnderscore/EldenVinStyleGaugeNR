#include "overlay.h"
#include "style_rank.h"
#include "config.h"
#include "d3dhook.h"
#include "memory.h"
#include "game_textures.h"
#include "buff_timers.h"
#include "convenience_menu.h"
#include "hotkey_sheet.h"
#include <Windows.h>
#include <imgui.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern void Log(const char* fmt, ...);

namespace Overlay {

static GaugeConfig g_Config;
static bool g_ConfigLoaded = false;
static bool g_WasPositionMode = false;
static bool g_SnapPosition = false;
static bool g_SnapBuffs = false;

static ImU32 ApplyOpacity(ImU32 color, float opacity) {
    int a = (int)(((color >> 24) & 0xFF) * opacity);
    return (color & 0x00FFFFFF) | ((ImU32)a << 24);
}

// Extract RGBA components
static void ColorComponents(ImU32 c, int& r, int& g, int& b, int& a) {
    r = (c >> 0) & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF; a = (c >> 24) & 0xFF;
}

// Blend two colors
static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    int ar, ag, ab, aa, br, bg, bb, ba;
    ColorComponents(a, ar, ag, ab, aa);
    ColorComponents(b, br, bg, bb, ba);
    return IM_COL32(
        ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t),
        ab + (int)((bb - ab) * t), aa + (int)((ba - aa) * t));
}

// Brighten a color
static ImU32 Brighten(ImU32 c, float amount) {
    int r, g, b, a;
    ColorComponents(c, r, g, b, a);
    r = (int)fminf(255, r + 255 * amount);
    g = (int)fminf(255, g + 255 * amount);
    b = (int)fminf(255, b + 255 * amount);
    return IM_COL32(r, g, b, a);
}

// Darken a color
static ImU32 Darken(ImU32 c, float amount) {
    int r, g, b, a;
    ColorComponents(c, r, g, b, a);
    r = (int)fmaxf(0, r * (1.0f - amount));
    g = (int)fmaxf(0, g * (1.0f - amount));
    b = (int)fmaxf(0, b * (1.0f - amount));
    return IM_COL32(r, g, b, a);
}

// Core rank colors (fully opaque base)
struct RankStyle {
    ImU32 primary;
    ImU32 glow;     // lighter version for bloom
    ImU32 dark;     // darker version for shadows
};

static RankStyle GetRankStyle(int rank) {
    static const ImU32 BASE[] = {
        IM_COL32(100, 100, 100, 255), // 0 None
        IM_COL32(160, 160, 170, 255), // 1 D  - steel gray
        IM_COL32(70, 160, 255, 255),  // 2 C  - cold blue
        IM_COL32(50, 210, 80, 255),   // 3 B  - emerald
        IM_COL32(255, 210, 30, 255),  // 4 A  - gold
        IM_COL32(255, 120, 0, 255),   // 5 S  - deep orange
        IM_COL32(255, 50, 50, 255),   // 6 SS - crimson
        IM_COL32(180, 40, 255, 255),  // 7 SSS - violet
        IM_COL32(160, 50, 240, 255),  // 8 V  - royal purple
    };
    RankStyle s;
    s.primary = BASE[rank];
    s.glow = Brighten(BASE[rank], 0.4f);
    s.dark = Darken(BASE[rank], 0.5f);
    return s;
}

static const char* RANK_NAMES[] = { "", "D", "C", "B", "A", "S", "SS", "SSS", "V" };
static const char* SEGMENT_LABELS[] = { "D", "C", "B", "A", "S", "SS", "SSS", "V" };

// Fast pseudo-random for visual effects (not crypto, just needs to look random)
static uint32_t s_RngState = 12345;
static void SeedRng(uint32_t seed) { s_RngState = seed | 1; }
static uint32_t Rng() { s_RngState ^= s_RngState << 13; s_RngState ^= s_RngState >> 17; s_RngState ^= s_RngState << 5; return s_RngState; }
static float RngFloat() { return (float)(Rng() & 0xFFFF) / 65535.0f; }   // 0..1
static float RngFloat(float lo, float hi) { return lo + RngFloat() * (hi - lo); }

// Draw a lightning bolt from (x1,y1) to (x2,y2) with jitter
static void DrawBolt(ImDrawList* draw, float x1, float y1, float x2, float y2,
                     ImU32 color, int segments, float jitter, float thickness) {
    float dx = (x2 - x1) / segments;
    float dy = (y2 - y1) / segments;
    float px = x1, py = y1;
    for (int i = 1; i <= segments; i++) {
        float nx, ny;
        if (i == segments) {
            nx = x2; ny = y2;
        } else {
            // Perpendicular jitter
            float len = sqrtf(dx * dx + dy * dy);
            float perpX = -dy / (len + 0.001f);
            float perpY = dx / (len + 0.001f);
            float offset = RngFloat(-jitter, jitter);
            nx = x1 + dx * i + perpX * offset;
            ny = y1 + dy * i + perpY * offset;
        }
        draw->AddLine(ImVec2(px, py), ImVec2(nx, ny), color, thickness);
        px = nx; py = ny;
    }
}

// Elden Ring HUD palette, sampled from the game's own bar sprites in 01_common.tpf
// (SB_FE_01: MENU_FL_BarDeco for the gold trim, MENU_FL_Condition_Bar for the fill shading)
static const ImU32 ER_GOLD       = IM_COL32(146, 139, 108, 255);
static const ImU32 ER_GOLD_LIGHT = IM_COL32(181, 173, 132, 255);
static const ImU32 ER_GOLD_DARK  = IM_COL32(92, 84, 70, 255);
static const ImU32 ER_BASE       = IM_COL32(14, 14, 12, 210);

// Fill colour climbs with the charge: ash grey -> glintstone blue -> frost silver -> grace gold
// -> deep amber, then white-gold once the Ultimate is ready (blue reaches gold through silver,
// since blending them directly goes muddy)
static ImU32 UltTint(float fill) {
    static const struct { float at; ImU32 col; } STOPS[] = {
        { 0.00f, IM_COL32(128, 126, 122, 255) },
        { 0.30f, IM_COL32(120, 160, 232, 255) },
        { 0.50f, IM_COL32(220, 226, 236, 255) },
        { 0.75f, IM_COL32(232, 190, 98, 255) },
        { 0.95f, IM_COL32(238, 160, 62, 255) },
        { 1.00f, IM_COL32(255, 242, 205, 255) },
    };
    const int n = sizeof(STOPS) / sizeof(STOPS[0]);
    for (int i = 1; i < n; i++) {
        if (fill <= STOPS[i].at)
            return LerpColor(STOPS[i - 1].col, STOPS[i].col, (fill - STOPS[i - 1].at) / (STOPS[i].at - STOPS[i - 1].at));
    }
    return STOPS[n - 1].col;
}

// Black-and-white crackle like the Ultimate's own lightning: starts at 60% charge, builds toward
// full, and arcs along the whole bar once ready. Bolts re-roll ~20 times a second.
static void DrawUltLightning(ImDrawList* draw, float x, float y, float w, float h, float fill, float op) {
    float fw = w * fill;
    float k = (fill - 0.6f) / 0.4f;
    if (k <= 0.0f || fw < 4.0f) return;
    if (k > 1.0f) k = 1.0f;
    bool ready = fill >= 1.0f;
    float t = (float)ImGui::GetTime();
    SeedRng((uint32_t)(t * 20.0f) * 2654435761u);

    int bolts = ready ? 8 : 1 + (int)(k * 4.0f);
    for (int b = 0; b < bolts; b++) {
        if (RngFloat() < (ready ? 0.25f : 0.5f)) continue;
        float sx, sy, ex, ey, jitter;
        if (ready && (Rng() & 3) == 0) {
            // Arc running along the fill
            sx = x + RngFloat(0.0f, fw * 0.8f);
            sy = y + h * RngFloat(0.3f, 0.7f);
            ex = fminf(x + fw, sx + RngFloat(h * 2.0f, h * 5.0f));
            ey = y + h * RngFloat(0.3f, 0.7f);
            jitter = h * 0.35f;
        } else {
            // Spark off the top or bottom edge
            bool top = Rng() & 1;
            sx = x + RngFloat(fw * 0.15f, fw);
            sy = top ? y : y + h;
            float len = h * RngFloat(0.4f, 0.8f + k * 0.8f);
            ex = sx + RngFloat(-h, h);
            ey = top ? sy - len : sy + len;
            jitter = h * (0.25f + 0.25f * k);
        }
        uint32_t seed = Rng();
        float a = RngFloat(0.6f, 1.0f) * op;
        // Same path twice: dark outline, then the white core
        SeedRng(seed);
        DrawBolt(draw, sx, sy, ex, ey, IM_COL32(0, 0, 0, (int)(170 * a)), 4, jitter, 3.0f);
        SeedRng(seed);
        DrawBolt(draw, sx, sy, ex, ey, IM_COL32(255, 255, 255, (int)(235 * a)), 4, jitter, 1.2f);
        if (RngFloat() < 0.3f + 0.3f * k) {
            float mx = (sx + ex) * 0.5f, my = (sy + ey) * 0.5f;
            float bx = mx + RngFloat(-h, h), by = my + (ey - sy) * RngFloat(0.2f, 0.5f);
            uint32_t bseed = Rng();
            SeedRng(bseed);
            DrawBolt(draw, mx, my, bx, by, IM_COL32(0, 0, 0, (int)(120 * a)), 3, jitter * 0.6f, 2.5f);
            SeedRng(bseed);
            DrawBolt(draw, mx, my, bx, by, IM_COL32(255, 255, 255, (int)(170 * a)), 3, jitter * 0.6f, 1.0f);
        }
        SeedRng(seed ^ 0x9E3779B9u);
    }
}

static float s_UltTrail = 0.0f;     // drain trail, like the HP bar's damage trail
static float s_UltTrailHold = 0.0f;

static void HLine(ImDrawList* draw, float x0, float x1, float y, ImU32 col) {
    draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 1.0f), col);
}

// Elden Vins Ultimate charge, drawn as a fourth HUD bar under HP/FP/stamina:
// dark base, gold trim, grainy pale fill (the Ultimate's lightning is black and white).
// No text; a full bar shimmers and its end caps pulse.
static void DrawUltimateBar(ImDrawList* draw, float x, float y, float w, float h, float fill, float op) {
    x = floorf(x); y = floorf(y);
    float t = (float)ImGui::GetTime();
    float dt = ImGui::GetIO().DeltaTime;
    bool ready = fill >= 1.0f;

    // Trail holds briefly, then slides down to the real value
    if (fill >= s_UltTrail) { s_UltTrail = fill; s_UltTrailHold = 0.6f; }
    else if ((s_UltTrailHold -= dt) <= 0.0f) s_UltTrail = fmaxf(fill, s_UltTrail - dt * 0.35f);

    float y0 = y + 1.0f, y1 = y + h - 1.0f;
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ApplyOpacity(ER_BASE, op));

    float fw = floorf(w * fill);
    float tw = floorf(w * s_UltTrail);
    if (tw > fw)
        draw->AddRectFilled(ImVec2(x + fw, y0), ImVec2(x + tw, y1), ApplyOpacity(IM_COL32(150, 140, 110, 140), op));

    if (fw > 0.0f) {
        // Shaded like the game's fill sprite: dimmer top, bright band low down, darker bottom edge
        float mid = y0 + (y1 - y0) * 0.72f;
        ImU32 tint = UltTint(fill);
        ImU32 top = ApplyOpacity(Darken(tint, 0.35f), op);
        ImU32 band = ApplyOpacity(tint, op);
        ImU32 bottom = ApplyOpacity(Darken(tint, 0.5f), op);
        draw->AddRectFilledMultiColor(ImVec2(x, y0), ImVec2(x + fw, mid), top, top, band, band);
        draw->AddRectFilledMultiColor(ImVec2(x, mid), ImVec2(x + fw, y1), band, band, bottom, bottom);

        // Fixed speckle so the fill reads as the game's textured metal rather than flat colour
        SeedRng(0x5EED);
        for (float gx = x; gx < x + fw; gx += 2.0f) {
            float gy = RngFloat(y0, y1 - 2.0f);
            float gl = RngFloat(1.0f, (y1 - y0) * 0.6f);
            draw->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + 2.0f, gy + gl),
                IM_COL32(40, 40, 45, (int)(RngFloat(0.0f, 45.0f) * op)));
        }

        if (ready) {
            float cx = x + w * (fmodf(t * 0.6f, 1.6f) - 0.3f);
            float bw = w * 0.18f;
            ImU32 clear = IM_COL32(255, 255, 255, 0);
            ImU32 lit = ApplyOpacity(IM_COL32(255, 255, 255, 110), op);
            draw->PushClipRect(ImVec2(x, y0), ImVec2(x + fw, y1), true);
            draw->AddRectFilledMultiColor(ImVec2(cx - bw, y0), ImVec2(cx, y1), clear, lit, lit, clear);
            draw->AddRectFilledMultiColor(ImVec2(cx, y0), ImVec2(cx + bw, y1), lit, clear, clear, lit);
            draw->PopClipRect();
        }
    }

    // Gold trim as in MENU_FL_BarDeco: light line against the bar, darker line outside it
    HLine(draw, x - 1, x + w + 1, y - 1, ApplyOpacity(ER_GOLD_LIGHT, op * 0.9f));
    HLine(draw, x - 1, x + w + 1, y - 2, ApplyOpacity(ER_GOLD_DARK, op * 0.6f));
    HLine(draw, x - 1, x + w + 1, y + h, ApplyOpacity(ER_GOLD, op * 0.9f));
    HLine(draw, x - 1, x + w + 1, y + h + 1, ApplyOpacity(ER_GOLD_DARK, op * 0.6f));

    float glow = ready ? 0.5f + 0.5f * sinf(t * 3.0f) : 0.0f;
    ImU32 cap = ApplyOpacity(LerpColor(ER_GOLD, IM_COL32(240, 232, 190, 255), glow), op);
    draw->AddRectFilled(ImVec2(x - 3, y - 3), ImVec2(x - 1, y + h + 3), cap);
    draw->AddRectFilled(ImVec2(x + w + 1, y - 3), ImVec2(x + w + 3, y + h + 3), cap);

    DrawUltLightning(draw, x, y, w, h, fill, op);
}

// The game's own HUD bar pieces, from SB_FE_01 in menu/hi/01_common.tpf (rects from 01_common.sblytbnd).
// Loaded from Elden Vins' copy of that file in the pack, which sits next to this DLL's folder.
struct Sprite { float x, y, w, h; };
static const Sprite SPR_BASE   { 0, 29, 2926, 44 };  // MENU_FL_HP_Base: translucent dark band
static const Sprite SPR_FILL   { 0, 469, 820, 26 };  // MENU_FL_Condition_Bar: white grain, tinted per bar
static const Sprite SPR_TIP    { 2142, 453, 98, 40 }; // MENU_FL_BarEdge2: glowing leading edge
static const Sprite SPR_DECO   { 0, 0, 2938, 25 };   // MENU_FL_BarDeco: gold line, ornament at the right end
static const Sprite SPR_DECO_L { 1362, 397, 46, 38 }; // MENU_FL_BarDeco_L: ornament at the left end
static const Sprite SPR_REGAIN { 3172, 0, 32, 64 };  // MENU_FL_HP_Regain: soft glow

// [0] SB_FE_01 for the bar, then the SB_Status_* sheets with the buff icons
static constexpr int kMaxSheets = 4;
static std::vector<uint8_t> s_AtlasDds[kMaxSheets];
static std::atomic<int> s_AtlasState{ 0 }; // 0 idle, 1 loading, 2 ready to upload, 3 uploaded, -1 unavailable
static ImTextureID s_Atlas = 0;
static float s_AtlasW = 1.0f, s_AtlasH = 1.0f;
static BuffTimers::Sheets s_StatusSheets;
static std::vector<uint8_t> s_ItemIconsDds;

static DWORD WINAPI LoadAtlasThread(LPVOID) {
    char path[MAX_PATH];
    GameTextures::PathNextToDll("..\\elden-vins\\menu\\hi\\01_common.tpf.dcx", path, sizeof(path));
    const wchar_t* names[kMaxSheets] = { L"SB_FE_01" };
    int count = 1 + BuffTimers::SheetCount();
    if (count > kMaxSheets) count = kMaxSheets;
    for (int i = 1; i < count; i++) names[i] = BuffTimers::SheetName(i - 1);
    int found = GameTextures::LoadDdsMany(path, names, s_AtlasDds, count);

    // weapon buff icons: a plain DDS next to this DLL (generated, see er-buff-timers/gen_buff_icons.py)
    GameTextures::PathNextToDll(BuffTimers::ItemIconsFile(), path, sizeof(path));
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        s_ItemIconsDds.resize(size > 0 ? size : 0);
        if (size <= 0 || fread(s_ItemIconsDds.data(), 1, size, f) != (size_t)size) s_ItemIconsDds.clear();
        fclose(f);
    }
    if (s_ItemIconsDds.empty()) Log("Buff timers: can't read %s", path);
    s_AtlasState = found > 0 ? 2 : -1;
    return 0;
}

static ImTextureID Upload(std::vector<uint8_t>& dds, float& w, float& h) {
    ImTextureID tex = 0;
    if (dds.size() >= 0x14) {
        uint32_t dh, dw;
        memcpy(&dh, dds.data() + 0x0C, 4);
        memcpy(&dw, dds.data() + 0x10, 4);
        w = (float)dw;
        h = (float)dh;
        tex = D3DHook::CreateTextureFromDds(dds.data(), dds.size());
    }
    std::vector<uint8_t>().swap(dds);
    return tex;
}

// Load in the background on first use, upload on the render thread once the bytes are ready
static void UpdateAtlas() {
    int st = s_AtlasState;
    if (st == 0) {
        s_AtlasState = 1;
        HANDLE th = CreateThread(nullptr, 0, LoadAtlasThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th); else s_AtlasState = -1;
    } else if (st == 2) {
        s_Atlas = Upload(s_AtlasDds[0], s_AtlasW, s_AtlasH);
        for (int i = 0; i + 1 < kMaxSheets && i < 4; i++)
            s_StatusSheets.tex[i] = Upload(s_AtlasDds[i + 1], s_StatusSheets.w[i], s_StatusSheets.h[i]);
        s_StatusSheets.items = Upload(s_ItemIconsDds, s_StatusSheets.itemsW, s_StatusSheets.itemsH);
        s_AtlasState = 3; // Blit/buff icons check their own texture
    }
}

// Draw the part (sx0,sy0)-(sx1,sy1) of a sprite, in sprite pixels, into a screen rect
static void Blit(ImDrawList* draw, const Sprite& s, float sx0, float sy0, float sx1, float sy1,
                 float x0, float y0, float x1, float y1, ImU32 col) {
    if (x1 <= x0 || y1 <= y0) return;
    draw->AddImage(ImTextureRef(s_Atlas), ImVec2(x0, y0), ImVec2(x1, y1),
        ImVec2((s.x + sx0) / s_AtlasW, (s.y + sy0) / s_AtlasH),
        ImVec2((s.x + sx1) / s_AtlasW, (s.y + sy1) / s_AtlasH), col);
}

// Same bar as DrawUltimateBar, built from the game's sprites. Scale: the fill sprite is 26px tall.
static void DrawUltimateBarSprites(ImDrawList* draw, float x, float y, float w, float h, float fill, float op) {
    x = floorf(x); y = floorf(y);
    float s = h / 26.0f;
    float t = (float)ImGui::GetTime();
    float dt = ImGui::GetIO().DeltaTime;
    bool ready = fill >= 1.0f;
    ImU32 white = ApplyOpacity(IM_COL32_WHITE, op);

    if (fill >= s_UltTrail) { s_UltTrail = fill; s_UltTrailHold = 0.6f; }
    else if ((s_UltTrailHold -= dt) <= 0.0f) s_UltTrail = fmaxf(fill, s_UltTrail - dt * 0.35f);

    Blit(draw, SPR_BASE, 16, 8, 2908, 36, x - 2 * s, y - s, x + w + 2 * s, y + h + s, white);

    float fw = floorf(w * fill);
    float tw = floorf(w * s_UltTrail);
    if (tw > fw)
        Blit(draw, SPR_FILL, 820 * fill, 0, 820 * s_UltTrail, 26, x + fw, y, x + tw, y + h,
            ApplyOpacity(IM_COL32(200, 180, 120, 150), op));

    ImU32 tint = UltTint(fill);
    Blit(draw, SPR_FILL, 0, 0, 820 * fill, 26, x, y, x + fw, y + h, ApplyOpacity(tint, op));

    if (ready) {
        float cx = x + w * (fmodf(t * 0.6f, 1.6f) - 0.3f);
        float bw = w * 0.18f;
        ImU32 clear = IM_COL32(255, 255, 255, 0);
        ImU32 lit = ApplyOpacity(IM_COL32(255, 255, 255, 90), op);
        draw->PushClipRect(ImVec2(x, y + 2 * s), ImVec2(x + fw, y + h - 2 * s), true);
        draw->AddRectFilledMultiColor(ImVec2(cx - bw, y), ImVec2(cx, y + h), clear, lit, lit, clear);
        draw->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + bw, y + h), lit, clear, clear, lit);
        draw->PopClipRect();
    }

    if (fw > 0.0f) {
        // Bright edge column (92) of the tip sits on the end of the fill
        float tipA = ready ? 0.75f + 0.25f * sinf(t * 3.0f) : 1.0f;
        draw->PushClipRect(ImVec2(x, y - 10 * s), ImVec2(x + fw + 6 * s, y + h + 10 * s), true);
        Blit(draw, SPR_TIP, 0, 0, 98, 40, x + fw - 92 * s, y - 9 * s, x + fw + 6 * s, y + 31 * s,
            ApplyOpacity(LerpColor(IM_COL32_WHITE, tint, 0.5f), op * tipA));
        draw->PopClipRect();
    }

    // Gold line just under the bar, its ornament end at the right, the left ornament at the start
    float ly = y + h + s - 15 * s;
    float end = 200 * s;
    float rx = x + w + 8 * s;
    Blit(draw, SPR_DECO, 0, 0, 2938 - 200, 25, x - 4 * s, ly, rx - end, ly + 25 * s, white);
    Blit(draw, SPR_DECO, 2938 - 200, 0, 2938, 25, rx - end, ly, rx, ly + 25 * s, white);
    Blit(draw, SPR_DECO_L, 0, 0, 46, 38, x - 30 * s, ly - 13 * s, x + 16 * s, ly + 25 * s, white);

    if (ready) {
        float g = 0.5f + 0.5f * sinf(t * 3.0f);
        float gs = h * 1.3f;
        Blit(draw, SPR_REGAIN, 0, 0, 32, 64, x + w - gs * 0.5f, y + h * 0.5f - gs, x + w + gs * 0.5f, y + h * 0.5f + gs,
            ApplyOpacity(tint, op * (0.5f + 0.5f * g)));
    }

    DrawUltLightning(draw, x, y, w, h, fill, op);
}

// Buff timers: drawn every frame on their own (the gauge below returns early when idle).
// In F7 mode the row gets a draggable frame, with sample icons if nothing is active.
static void DrawBuffTimers(bool posMode) {
    const Memory::GameLayout* game = Memory::Game();
    if (!g_Config.buffTimers || !game || strcmp(game->moduleName, "eldenring.exe") != 0) return;
    UpdateAtlas();

    std::vector<BuffTimers::Buff> buffs = BuffTimers::Update(g_Config.buffMinDuration);
    if (posMode && buffs.empty()) buffs = BuffTimers::Preview();
    if (buffs.empty()) return;

    auto& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x, screenH = io.DisplaySize.y;
    float scale = screenH / 1080.0f * g_Config.buffScale;
    int perRow = (int)g_Config.buffPerRow;
    float x = floorf(screenW * g_Config.buffPosX), y = floorf(screenH * g_Config.buffPosY);

    if (posMode) {
        ImVec2 size = BuffTimers::Measure(buffs.size(), scale, perRow);
        if (g_SnapBuffs) {
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
            g_SnapBuffs = false;
        }
        ImGui::SetNextWindowSize(ImVec2(size.x + 4, size.y + 4), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 15, 5, 100));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 180, 50, 200));
        ImGui::Begin("##BuffTimers", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing);
        ImVec2 wp = ImGui::GetWindowPos();
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        x = wp.x + 2; y = wp.y + 2;
        g_Config.buffPosX = x / screenW;
        g_Config.buffPosY = y / screenH;
    }

    BuffTimers::Draw(ImGui::GetForegroundDrawList(), D3DHook::TimerFont(), s_StatusSheets, buffs,
        x, y, scale, perRow, g_Config.buffOpacity);
}

static void FinishWindow(bool posMode, float screenW) {
    // Position mode hint
    if (posMode) {
        ImDrawList* bgDraw = ImGui::GetForegroundDrawList();
        const char* hint = "F7: Drag the gauge and the buff timers to reposition. Press F7 again to lock & save.";
        ImVec2 hintSize = ImGui::CalcTextSize(hint);
        float hx = (screenW - hintSize.x) * 0.5f;
        bgDraw->AddRectFilled(ImVec2(hx - 8, 8), ImVec2(hx + hintSize.x + 8, 32), IM_COL32(0, 0, 0, 180), 4.0f);
        bgDraw->AddText(ImVec2(hx, 12), IM_COL32(255, 200, 80, 255), hint);
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void Render(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue) {
    if (!g_ConfigLoaded) {
        g_Config = Config::Load();
        g_ConfigLoaded = true;
    }

    bool posMode = D3DHook::IsPositionMode();

    // Detect transitions into/out of position mode
    if (!g_WasPositionMode && posMode) {
        g_SnapPosition = true; // snap window to config pos on entering
        g_SnapBuffs = true;
    }
    if (g_WasPositionMode && !posMode) {
        Log("Position mode ended, saving config");
        Config::Save(g_Config);
    }
    g_WasPositionMode = posMode;

    DrawBuffTimers(posMode);
    if (Memory::Game() && strcmp(Memory::Game()->moduleName, "eldenring.exe") == 0) {
        HotkeySheet::Draw(D3DHook::TimerFont(), D3DHook::IsHotkeySheetOpen());
        ConvenienceMenu::Draw(D3DHook::TimerFont(), D3DHook::IsMenuOpen());
    }

    StyleRankState state = StyleRank::Update();
    int rank = state.rank;

    // Elden Ring shows the Ultimate charge bar (style ranks there never fire)
    bool ultMode = Memory::Game() && Memory::Game()->ultEffectId != 0;
    if (ultMode) UpdateAtlas();
    if (ultMode && state.fill < 0.0f) {
        s_UltTrail = 0.0f;
        if (posMode) state.fill = 0.6f; // preview while dragging
    }

    // In position mode, always show the gauge (use a fake rank for preview)
    int displayRank = rank;
    if (posMode && displayRank <= 0) displayRank = 5; // show "S" as preview

    if (displayRank <= 0) return;

    float op = g_Config.opacity;
    auto& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    float gw = g_Config.gaugeWidth;
    float gh = g_Config.gaugeHeight;
    float windowW = gw + 40.0f;
    float windowH = gh + 60.0f + (g_Config.showLabels ? 16.0f : 0.0f);

    if (posMode) {
        // In position mode: let ImGui handle positioning (draggable)
        // Snap to config position on first frame of each position mode entry
        if (g_SnapPosition) {
            float posX = screenW * g_Config.posX - windowW * 0.5f;
            float posY = screenH * g_Config.posY - windowH * 0.5f;
            ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
            g_SnapPosition = false;
        }
        ImGui::SetNextWindowSize(ImVec2(windowW, windowH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.3f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 15, 5, 100));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 180, 50, 200));
        ImGui::Begin("##StyleRank", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing);

        // Update config from current window position
        ImVec2 wp = ImGui::GetWindowPos();
        g_Config.posX = (wp.x + windowW * 0.5f) / screenW;
        g_Config.posY = (wp.y + windowH * 0.5f) / screenH;
    } else {
        // Normal mode: locked, invisible, no input
        float posX = screenW * g_Config.posX - windowW * 0.5f;
        float posY = screenH * g_Config.posY - windowH * 0.5f;

        ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(windowW, windowH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::Begin("##StyleRank", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground);
    }

    rank = displayRank;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float centerX = wp.x + windowW * 0.5f;

    if (ultMode && state.fill >= 0.0f) {
        float bx = wp.x + 20.0f, by = wp.y + (windowH - gh) * 0.5f;
        if (s_AtlasState == 3 && s_Atlas) DrawUltimateBarSprites(draw, bx, by, gw, gh, state.fill, op);
        else DrawUltimateBar(draw, bx, by, gw, gh, state.fill, op);
        FinishWindow(posMode, screenW);
        return;
    }

    RankStyle rs = GetRankStyle(rank);
    const char* rankName = state.label ? state.label : RANK_NAMES[rank];

    // === RANK LETTER with glow ===
    ImGui::SetWindowFontScale(g_Config.fontSize);
    ImVec2 textSize = ImGui::CalcTextSize(rankName);
    float textX = centerX - textSize.x * 0.5f;
    float textY = wp.y + 2.0f;

    // Glow layers (multiple offset copies with decreasing alpha)
    for (int layer = 3; layer >= 1; layer--) {
        float spread = (float)layer * 1.5f;
        int alpha = (int)(40.0f / layer * op);
        ImU32 glowCol = (rs.glow & 0x00FFFFFF) | ((ImU32)alpha << 24);
        draw->AddText(ImVec2(textX - spread, textY), glowCol, rankName);
        draw->AddText(ImVec2(textX + spread, textY), glowCol, rankName);
        draw->AddText(ImVec2(textX, textY - spread), glowCol, rankName);
        draw->AddText(ImVec2(textX, textY + spread), glowCol, rankName);
    }

    // Drop shadow
    draw->AddText(ImVec2(textX + 2, textY + 2), ApplyOpacity(IM_COL32(0, 0, 0, 200), op), rankName);
    // Main letter
    draw->AddText(ImVec2(textX, textY), ApplyOpacity(rs.primary, op), rankName);
    // Bright highlight (slight upward offset for top-lit effect)
    draw->AddText(ImVec2(textX, textY - 0.5f), ApplyOpacity(Brighten(rs.primary, 0.2f), op * 0.5f), rankName);

    ImGui::SetWindowFontScale(1.0f);

    if (g_Config.showBar) {
        float barX = wp.x + 20.0f;
        float barY = wp.y + textSize.y + 8.0f;
        float rounding = gh * 0.35f; // capsule-ish rounding

        // === OUTER GLOW (soft halo behind the bar) ===
        float fillWidth = gw * (state.fill >= 0.0f ? state.fill : (float)rank / 8.0f);
        if (fillWidth > 2) {
            ImU32 outerGlow = (rs.glow & 0x00FFFFFF) | ((ImU32)(int)(20 * op) << 24);
            draw->AddRectFilled(
                ImVec2(barX - 3, barY - 3),
                ImVec2(barX + fillWidth + 3, barY + gh + 3),
                outerGlow, rounding + 2);
        }

        // === BAR BACKGROUND (dark inset) ===
        // Outer dark border
        draw->AddRectFilled(
            ImVec2(barX - 1, barY - 1),
            ImVec2(barX + gw + 1, barY + gh + 1),
            ApplyOpacity(IM_COL32(10, 10, 10, 220), op), rounding + 1);

        // Inner background gradient (dark at bottom, slightly lighter at top)
        draw->AddRectFilledMultiColor(
            ImVec2(barX, barY),
            ImVec2(barX + gw, barY + gh),
            ApplyOpacity(IM_COL32(35, 32, 30, 200), op),  // top-left
            ApplyOpacity(IM_COL32(35, 32, 30, 200), op),  // top-right
            ApplyOpacity(IM_COL32(12, 10, 10, 200), op),  // bottom-right
            ApplyOpacity(IM_COL32(12, 10, 10, 200), op));  // bottom-left

        // === FILL BAR (gradient + shine + pulse) ===
        float t = (float)ImGui::GetTime();

        // Pulse intensity scales with rank (subtle at low, strong at high)
        float pulseSpeed = 2.0f + rank * 0.5f;
        float pulseAmount = 0.03f + rank * 0.015f; // 0.03 at D, ~0.15 at V
        float pulse = sinf(t * pulseSpeed) * pulseAmount;

        if (fillWidth > 2) {
            float fx1 = barX + 1;
            float fy1 = barY + 1;
            float fx2 = barX + fillWidth - 1;
            float fy2 = barY + gh - 1;

            // Pulsing fill: modulate brightness
            ImU32 pulsedPrimary = Brighten(rs.primary, pulse);
            ImU32 pulsedDark = Brighten(rs.dark, pulse * 0.5f);
            ImU32 fillTop = ApplyOpacity(pulsedPrimary, op);
            ImU32 fillBot = ApplyOpacity(pulsedDark, op);
            draw->AddRectFilledMultiColor(
                ImVec2(fx1, fy1), ImVec2(fx2, fy2),
                fillTop, fillTop, fillBot, fillBot);

            // Top shine line (pulses slightly)
            float shineAlpha = 0.5f + pulse * 2.0f;
            ImU32 shineColor = ApplyOpacity(Brighten(rs.primary, 0.5f), op * shineAlpha);
            draw->AddRectFilled(
                ImVec2(fx1 + 2, fy1),
                ImVec2(fx2 - 2, fy1 + 2),
                shineColor, 1.0f);

            // Leading edge glow (pulses)
            if (rank < 8) {
                float edgeX = fx2;
                float edgeAlpha = 0.6f + sinf(t * pulseSpeed * 1.5f) * 0.3f;
                ImU32 edgeGlow = ApplyOpacity(rs.glow, op * edgeAlpha);
                ImU32 edgeFade = (rs.glow & 0x00FFFFFF);
                draw->AddRectFilledMultiColor(
                    ImVec2(edgeX - 10, fy1), ImVec2(edgeX, fy2),
                    edgeFade, edgeGlow, edgeGlow, edgeFade);
            }
        }

        // === FRAME BORDER (thin metallic) ===
        ImU32 frameDark = ApplyOpacity(IM_COL32(60, 55, 50, 160), op);
        ImU32 frameLight = ApplyOpacity(IM_COL32(100, 95, 85, 100), op);
        draw->AddRect(
            ImVec2(barX, barY), ImVec2(barX + gw, barY + gh),
            frameDark, rounding, 0, 1.0f);
        // Subtle inner highlight at top edge
        draw->AddLine(
            ImVec2(barX + rounding, barY + 0.5f),
            ImVec2(barX + gw - rounding, barY + 0.5f),
            frameLight);

        // === SEGMENT DIVIDERS (notch style) ===
        float notchLen = gh * 0.25f;
        for (int i = 1; i < 8; i++) {
            float tickX = barX + gw * (float)i / 8.0f;
            ImU32 tickColor = (i <= rank)
                ? ApplyOpacity(IM_COL32(0, 0, 0, 120), op)
                : ApplyOpacity(IM_COL32(80, 75, 70, 60), op);
            // Top notch
            draw->AddLine(ImVec2(tickX, barY), ImVec2(tickX, barY + notchLen), tickColor);
            // Bottom notch
            draw->AddLine(ImVec2(tickX, barY + gh - notchLen), ImVec2(tickX, barY + gh), tickColor);
        }

        // === SEGMENT LABELS ===
        if (g_Config.showLabels && !state.label) {
            for (int i = 0; i < 8; i++) {
                float lx = barX + gw * (float)i / 8.0f + gw / 16.0f;
                ImU32 labelColor;
                if (i < rank) {
                    // Lit labels match rank color but subdued
                    labelColor = ApplyOpacity(Darken(rs.primary, 0.2f), op * 0.7f);
                } else if (i == rank) {
                    // Next rank: slightly visible
                    labelColor = ApplyOpacity(IM_COL32(80, 75, 70, 130), op);
                } else {
                    labelColor = ApplyOpacity(IM_COL32(40, 38, 35, 100), op);
                }
                ImVec2 lsize = ImGui::CalcTextSize(SEGMENT_LABELS[i]);
                // Shadow
                draw->AddText(ImVec2(lx - lsize.x * 0.5f + 1, barY + gh + 3.0f),
                    ApplyOpacity(IM_COL32(0, 0, 0, 150), op), SEGMENT_LABELS[i]);
                draw->AddText(ImVec2(lx - lsize.x * 0.5f, barY + gh + 2.0f),
                    labelColor, SEGMENT_LABELS[i]);
            }
        }

        // === ELECTRICITY CRACKLING (rank S and above) ===
        if (rank >= 5 && fillWidth > 2) {
            // Seed RNG from time so bolts change each frame
            SeedRng((uint32_t)(t * 1000) ^ (uint32_t)(t * 7919));

            // Number of bolts scales with rank: S=2, SS=3, SSS=5, V=7
            int boltCount = (rank - 5) * 2 + 2;
            if (rank == 8) boltCount = 7;

            float boltJitter = gh * 0.8f + (rank - 5) * 2.0f;
            float boltThickness = 1.0f + (rank >= 7 ? 0.5f : 0.0f);

            for (int b = 0; b < boltCount; b++) {
                if (RngFloat() < 0.35f) continue;

                float startX = barX + RngFloat(fillWidth * 0.2f, fillWidth);
                bool fromTop = Rng() & 1;
                float startY = fromTop ? (barY - 1) : (barY + gh + 1);

                float endX = startX + RngFloat(-15.0f, 15.0f);
                float endY = fromTop
                    ? (startY - RngFloat(4.0f, gh * 1.2f))
                    : (startY + RngFloat(4.0f, gh * 1.2f));

                int boltAlpha = (int)(RngFloat(120, 220) * op);
                ImU32 boltColor = (rs.glow & 0x00FFFFFF) | ((ImU32)boltAlpha << 24);

                DrawBolt(draw, startX, startY, endX, endY, boltColor, 4, boltJitter, boltThickness);

                if (RngFloat() < 0.3f) {
                    float midX = (startX + endX) * 0.5f + RngFloat(-3, 3);
                    float midY = (startY + endY) * 0.5f;
                    float branchX = midX + RngFloat(-12, 12);
                    float branchY = midY + RngFloat(-6, 6);
                    int brAlpha = boltAlpha / 2;
                    ImU32 branchColor = (rs.glow & 0x00FFFFFF) | ((ImU32)brAlpha << 24);
                    DrawBolt(draw, midX, midY, branchX, branchY, branchColor, 3, boltJitter * 0.6f, 1.0f);
                }
            }

            // V rank: extra ambient glow pulse
            if (rank == 8) {
                float vPulse = sinf(t * 4.0f) * 0.5f + 0.5f;
                int glowAlpha = (int)(vPulse * 30 * op);
                ImU32 vGlow = (rs.glow & 0x00FFFFFF) | ((ImU32)glowAlpha << 24);
                draw->AddRectFilled(
                    ImVec2(barX - 5, barY - 5),
                    ImVec2(barX + gw + 5, barY + gh + 5),
                    vGlow, rounding + 3);
            }
        }
    }

    FinishWindow(posMode, screenW);
}

} // namespace Overlay
