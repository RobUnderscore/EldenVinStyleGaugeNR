#include "overlay.h"
#include "style_rank.h"
#include "config.h"
#include "d3dhook.h"
#include <imgui.h>

extern void Log(const char* fmt, ...);

namespace Overlay {

static GaugeConfig g_Config;
static bool g_ConfigLoaded = false;
static bool g_WasPositionMode = false;

static ImU32 ApplyOpacity(ImU32 color, float opacity) {
    int a = (int)(((color >> 24) & 0xFF) * opacity);
    return (color & 0x00FFFFFF) | ((ImU32)a << 24);
}

static const ImU32 RANK_COLORS[] = {
    IM_COL32(128, 128, 128, 200), // 0 None
    IM_COL32(180, 180, 180, 220), // 1 D
    IM_COL32(100, 180, 255, 220), // 2 C
    IM_COL32(80, 220, 80, 220),   // 3 B
    IM_COL32(255, 220, 50, 220),  // 4 A
    IM_COL32(255, 140, 0, 230),   // 5 S
    IM_COL32(255, 80, 80, 240),   // 6 SS
    IM_COL32(200, 50, 255, 240),  // 7 SSS
    IM_COL32(255, 255, 255, 255), // 8 V
};

static const char* RANK_NAMES[] = { "", "D", "C", "B", "A", "S", "SS", "SSS", "V" };
static const char* SEGMENT_LABELS[] = { "D", "C", "B", "A", "S", "SS", "SSS", "V" };

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

    ImU32 rankColor = ApplyOpacity(RANK_COLORS[rank], op);
    const char* rankName = RANK_NAMES[rank];

    // Rank letter with shadow
    ImGui::SetWindowFontScale(g_Config.fontSize);
    ImVec2 textSize = ImGui::CalcTextSize(rankName);
    float textX = centerX - textSize.x * 0.5f;
    float textY = wp.y + 2.0f;
    draw->AddText(ImVec2(textX + 2, textY + 2), ApplyOpacity(IM_COL32(0, 0, 0, 220), op), rankName);
    draw->AddText(ImVec2(textX, textY), rankColor, rankName);
    ImGui::SetWindowFontScale(1.0f);

    if (g_Config.showBar) {
        float barX = wp.x + 20.0f;
        float barY = wp.y + textSize.y + 8.0f;

        // Background
        draw->AddRectFilled(
            ImVec2(barX, barY),
            ImVec2(barX + gw, barY + gh),
            ApplyOpacity(IM_COL32(20, 20, 20, 180), op), 4.0f);

        // Filled segments
        float fillWidth = gw * (float)rank / 8.0f;
        if (fillWidth > 0) {
            draw->AddRectFilled(
                ImVec2(barX + 1, barY + 1),
                ImVec2(barX + 1 + fillWidth - 2, barY + gh - 1),
                rankColor, 3.0f);
        }

        // Border (optional)
        if (g_Config.showBorder) {
            draw->AddRect(
                ImVec2(barX, barY),
                ImVec2(barX + gw, barY + gh),
                ApplyOpacity(IM_COL32(200, 200, 200, 80), op), 4.0f);
        }

        // Segment dividers
        for (int i = 1; i < 8; i++) {
            float tickX = barX + gw * (float)i / 8.0f;
            draw->AddLine(
                ImVec2(tickX, barY),
                ImVec2(tickX, barY + gh),
                ApplyOpacity(IM_COL32(100, 100, 100, 80), op));
        }

        // Segment labels
        if (g_Config.showLabels) {
            for (int i = 0; i < 8; i++) {
                float lx = barX + gw * (float)i / 8.0f + gw / 16.0f;
                ImU32 labelColor = (i < rank)
                    ? ApplyOpacity(rankColor, 0.8f)
                    : ApplyOpacity(IM_COL32(60, 60, 60, 150), op);
                ImVec2 lsize = ImGui::CalcTextSize(SEGMENT_LABELS[i]);
                draw->AddText(ImVec2(lx - lsize.x * 0.5f, barY + gh + 2.0f), labelColor, SEGMENT_LABELS[i]);
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
