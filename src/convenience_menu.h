#pragma once
#include "imgui.h"

// F8 menu that gives the player items listed in ConvenienceMenu.txt (next to the DLL).
// The file is re-read every time the menu opens, so entries can change without a rebuild.
namespace ConvenienceMenu {

// Draw the menu (call every frame; does nothing while closed)
void Draw(ImFont* font, bool open);

} // namespace ConvenienceMenu
