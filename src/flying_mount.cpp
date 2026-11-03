#include "flying_mount.h"
#include "world.h"
#include "imgui.h"
#include <Windows.h>
#include <atomic>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace FlyingMount {

using namespace World;

static constexpr uintptr_t WORLD_CHR_MAN = 0x3d6d018;  // +0x1e508 the main player
static constexpr uintptr_t MAIN_PLAYER = 0x1e508;
static constexpr int32_t kOnEffect = 100910;
static constexpr ULONGLONG kRefreshMs = 500;
// ChrIns apply SpEffect (chr, id, dont_sync), as fromsoftware-rs's ChrInsExt::apply_speffect
static constexpr uintptr_t APPLY_SPEFFECT = 0x3e8dc0;
static const uint8_t kApplyBytes[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xec, 0xc0 };

static std::atomic<bool> s_On{ false };
static int s_CodeOk = -1;  // -1 not checked yet
static ULONGLONG s_AppliedAt = 0;
static uintptr_t s_Player = 0;

bool Enabled() { return s_On; }

void SetEnabled(bool on) {
    if (s_On.exchange(on) != on) Log("FlyingMount: %s", on ? "on" : "off (the marker runs out within 2 s)");
}

static bool CodeOk() {
    if (s_CodeOk < 0) {
        const uint8_t* f = reinterpret_cast<const uint8_t*>(Base() + APPLY_SPEFFECT);
        __try {
            // another mod's jump in front is fine: we only call it
            s_CodeOk = !memcmp(f, kApplyBytes, sizeof(kApplyBytes)) ||
                (f[0] == 0xE9 && !memcmp(f + 5, kApplyBytes + 5, sizeof(kApplyBytes) - 5));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_CodeOk = 0;
        }
        Log("FlyingMount: apply-SpEffect routine %s", s_CodeOk ? "ok" : "NOT as expected; flying stays off");
    }
    return s_CodeOk == 1;
}

void RunFrame() {
    if (!s_On || !Base()) return;
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    uintptr_t player = wcm ? Ptr(wcm + MAIN_PLAYER) : 0;
    ULONGLONG now = GetTickCount64();
    if (player != s_Player) {  // a new character (after a load): give it 3 s
        s_Player = player;
        s_AppliedAt = now + 3000 - kRefreshMs;
        return;
    }
    if (!player || now < s_AppliedAt + kRefreshMs || !CodeOk()) return;
    s_AppliedAt = now;
    reinterpret_cast<void (*)(uintptr_t, int32_t, bool)>(Base() + APPLY_SPEFFECT)(player, kOnEffect, true);
}

void DrawMenuSection(float scale) {
    const ImVec4 gold(0.71f, 0.68f, 0.52f, 1.0f), dim(0.62f, 0.58f, 0.48f, 1.0f);
    ImGui::Spacing();
    ImGui::TextColored(gold, "Testing");
    ImGui::Separator();
    bool on = s_On;
    if (ImGui::Checkbox("Flying Torrent", &on)) SetEnabled(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only for you, and off again at every start. While Torrent sprints, tap sprint again:\n"
                          "Torrent climbs. In the air, jump to climb higher (again to keep climbing), run or\n"
                          "sprint to fly level, stand still to hover, walk to glide down; touching the ground\n"
                          "ends the flight. Switching this off in the air drops you, without fall damage.");
    if (s_CodeOk == 0) ImGui::TextColored(dim, "Not available: this game version's code isn't the one expected");
    (void)scale;
}

} // namespace FlyingMount
