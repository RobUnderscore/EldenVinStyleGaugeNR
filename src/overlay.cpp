#include "overlay.h"
#include "style_rank.h"
#include "config.h"
#include "d3dhook.h"
#include <imgui.h>
#include <cmath>

extern void Log(const char* fmt, ...);

namespace Overlay {

static GaugeConfig g_Config;
static bool g_ConfigLoaded = false;
static bool g_WasPositionMode = false;

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

void Render(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue) {
    if (!g_ConfigLoaded) {
        g_Config = Config::Load();
        g_ConfigLoaded = true;
    }

    bool posMode = D3DHook::IsPositionMode();

    // Detect transition out of position mode -> save new position
    if (g_WasPositionMode && !posMode) {
        Log("Position mode ended, saving config");
        Config::Save(g_Config);
    }
    g_WasPositionMode = posMode;

    StyleRankState state = StyleRank::Update();
    int rank = state.rank;

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
        // On first frame of position mode, snap to current config position
        static bool firstFrame = true;
        if (firstFrame || !g_WasPositionMode) {
            float posX = screenW * g_Config.posX - windowW * 0.5f;
            float posY = screenH * g_Config.posY - windowH * 0.5f;
            ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
            firstFrame = false;
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

    RankStyle rs = GetRankStyle(rank);
    const char* rankName = RANK_NAMES[rank];

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
        float fillWidth = gw * (float)rank / 8.0f;
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

        // Rounded corners clip (redraw corners with background)
        draw->AddRectFilled(
            ImVec2(barX, barY), ImVec2(barX + gw, barY + gh),
            IM_COL32(0, 0, 0, 0), rounding); // just for rounding

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
        if (g_Config.showLabels) {
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

    // Position mode hint
    if (posMode) {
        ImDrawList* bgDraw = ImGui::GetForegroundDrawList();
        const char* hint = "F7: Drag gauge to reposition. Press F7 again to lock & save.";
        ImVec2 hintSize = ImGui::CalcTextSize(hint);
        float hx = (screenW - hintSize.x) * 0.5f;
        bgDraw->AddRectFilled(ImVec2(hx - 8, 8), ImVec2(hx + hintSize.x + 8, 32), IM_COL32(0, 0, 0, 180), 4.0f);
        bgDraw->AddText(ImVec2(hx, 12), IM_COL32(255, 200, 80, 255), hint);
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace Overlay
