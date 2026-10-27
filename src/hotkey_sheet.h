#pragma once
#include "imgui.h"

// F6 panel listing the pack's hotkeys, read from Hotkeys.txt next to the DLL (re-read each
// time it opens). Display only: the game keeps its input while it's shown.
namespace HotkeySheet {

void Draw(ImFont* font, bool open);

} // namespace HotkeySheet
