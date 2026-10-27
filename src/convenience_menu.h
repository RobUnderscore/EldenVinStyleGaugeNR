#pragma once
#include "imgui.h"
#include <cstdint>

// F8 menu: the co-op resync section (coop_resync.h), then items listed in
// ConvenienceMenu.txt (next to the DLL). The file is re-read every time the menu opens, so
// entries can change without a rebuild.
namespace ConvenienceMenu {

// Draw the menu (call every frame; does nothing while closed)
void Draw(ImFont* font, bool open);

// Give the player an item through the game's ItemGib (id with its category bits: weapon 0, armor
// 0x10000000, talisman 0x20000000, goods 0x40000000, ash of war 0x80000000). False if it can't.
bool GiveItem(uint32_t id, int quantity);

} // namespace ConvenienceMenu
