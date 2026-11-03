#pragma once
#include "imgui.h"
#include <cstdint>
#include <string>
#include <vector>

// F8 menu: the co-op resync section (coop_resync.h), then items listed in
// ConvenienceMenu.txt (next to the DLL). The file is re-read every time the menu opens, so
// entries can change without a rebuild. With the native menu up (native_menu.h) the items are in
// Esc > System > Game Options > Vin-Swag instead, read once at startup.
namespace ConvenienceMenu {

// Draw the menu (call every frame; does nothing while closed)
void Draw(ImFont* font, bool open);

// One ConvenienceMenu.txt entry (id with its category bits)
struct Gift {
    std::string section;
    std::string label;
    uint32_t id = 0;
    int quantity = 1;
};

// The file's entries, read now (the native menu takes them once, at startup); false and why if it can't
bool ReadGifts(std::vector<Gift>& out, std::string& error);

// Give one now; what to tell the player ("Received ..." or why not)
std::string Receive(const Gift& gift);

// Give the player an item through the game's ItemGib (id with its category bits: weapon 0, armor
// 0x10000000, talisman 0x20000000, goods 0x40000000, ash of war 0x80000000). False if it can't.
bool GiveItem(uint32_t id, int quantity);

} // namespace ConvenienceMenu
