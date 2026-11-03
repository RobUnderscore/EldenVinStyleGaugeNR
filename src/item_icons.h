#pragma once
#include "imgui.h"

// The game's own item icons for Find an item: menu\hi\00_Solo (MENU_Knowledge_<icon id>, 1024 px BC7 each) out of the
// pack's Elden Vins files, read and shrunk to 128 px on a thread of their own and kept in one atlas, where the icons
// drawn least recently make room for new ones. Without those files (the rigs) there are no icons. Render thread only.
namespace ItemIcons {

// Draw an icon (an EquipParam* iconId) in a rect: false, drawing nothing, until it has been read (and for none)
bool Draw(ImDrawList* dl, int iconId, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE);

// Once a frame while icons are drawn: puts the ones read since into the atlas
void Update();

} // namespace ItemIcons
