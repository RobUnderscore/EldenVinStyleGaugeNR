#pragma once
#include "imgui.h"

// Elden Ring minimap (top right): the game's own world-map art around you, north up, with your co-op partners
// (where their overlay says they are), the graces you've found, your map beacons and the item you're tracking
// (Find an item). The tiles are read straight out of the game's Data0.bdt at the offsets MapTiles.txt (next to the
// DLL, built by er-drop-finder/maptiles.py from the game's own archive index) gives, decompressed with the game's
// Oodle on a thread of their own; positions, graces and beacons are read live on the game's task thread.
// The world-map layout follows kubilaiswf-minimap (MIT): 41x41 cells of 256 px at 1 px per metre, cell (i, j)
// centred on tile m60_{i+28}_{j+24}.
namespace Minimap {

// From CoopResync's per-frame game task
void RunFrame();

// Draw it (render thread, every frame)
void Draw(ImFont* font);

// Settings, kept in Minimap.ini in the user config folder (the Vin-Swag page in Game Options changes them).
// While a boss's health bar is up the minimap stays, fades to a quarter of its opacity, or hides.
enum BossMode { BOSS_KEEP = 0, BOSS_FADE = 1, BOSS_HIDE = 2 };
bool Enabled();
void SetEnabled(bool on);
int Opacity();  // percent, 10..100
void SetOpacity(int percent);
int BossFights();  // a BossMode
void SetBossFights(int mode);

} // namespace Minimap
