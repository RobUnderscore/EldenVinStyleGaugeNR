#pragma once

// Elden Ring: the overlay's fixed controls as a native "Vin-Swag" page in the game's own System menu (Game Options), through
// ERNativeUI (a shared native-menu host DLL the profile loads before ours; github.com/Flammrock/ERNativeUI, MIT,
// SDK headers in deps/ernativeui). Its menus are fixed at startup (API 1.3), so what changes while playing (the
// item finder, Go to <player>, the player and invader lists, tracking) stays in the F8 overlay. Without the host
// F8 keeps everything.
namespace NativeMenu {

// Start connecting on a thread of its own, as early as possible: ERNativeUI only takes menus in the first
// seconds of startup. Elden Ring only
void Start();

// The tab is registered: F8 leaves out what the tab has
bool Active();

} // namespace NativeMenu
