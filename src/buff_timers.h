#pragma once
#include <cstdint>
#include <vector>
#include "imgui.h"

namespace BuffTimers {

struct Buff {
    int32_t iconId;     // SpEffectParam.iconId = MENU_StatusIcon_<id> (unused when cell >= 0)
    float remaining;    // seconds
    float total;        // full duration, for the drain bar
    int16_t cell = -1;  // weapon buff: cell in BuffIcons.dds with the icon of what applied it
};

// Timed effects on the player that the HUD shows an icon for, plus weapon buffs (which the
// game only shows as a glow on the weapon), one per icon (the longest one wins when several
// share it), in the order they appeared. Effects shorter than minDuration are left out
// (procs that flicker on and off).
const std::vector<Buff>& Update(float minDuration);

// Where an icon sits in the SB_Status_* sheets; false if the game has no such sprite
bool IconRect(int32_t iconId, int& sheet, float& x, float& y, float& w, float& h);

// Number of SB_Status_* sheets and their texture names
int SheetCount();
const wchar_t* SheetName(int sheet);

struct Sheets {
    ImTextureID tex[4] = {};        // SB_Status_* sheets
    float w[4] = {}, h[4] = {};
    ImTextureID items = 0;          // BuffIcons.dds
    float itemsW = 1.0f, itemsH = 1.0f;
};

// File next to the DLL with the weapon buffs' item icons
const char* ItemIconsFile();

// Size of the block Draw() fills for count icons (for the F7 drag window)
ImVec2 Measure(size_t count, float scale, int perRow);

// A few real icons to show while positioning with nothing active
std::vector<Buff> Preview();

// Draw a row of icons with a drain bar and the time left under each, starting at (x, y).
// scale 1.0 = 1080p size; perRow icons before wrapping.
void Draw(ImDrawList* draw, ImFont* font, const Sheets& sheets, const std::vector<Buff>& buffs,
          float x, float y, float scale, int perRow, float opacity);

} // namespace BuffTimers
