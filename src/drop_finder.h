#pragma once
#include "imgui.h"
#include "world.h"
#include <string>

// Elden Ring "Find an item" (F8 menu, game 2.7.2.0): type a name, see every place the game hands that item
// out (pickups, enemy drops, shops, event and NPC rewards), whether you already have it from there, the nearest
// site of grace (found or not) and the closest one you can warp to; "Track" puts an arrow on screen. An item you
// no longer hold still shows as got: picked up (the flag of a lot sure to give it) or handed over to an NPC.
// Names, drop tables, shop stock, graces and flags are read live from the game, so they follow whatever mods
// are loaded. Placements come from DropFinder.txt next to the DLL (built from the game's map, event and talk
// files with the pack's mods by er-drop-finder/), since the game only keeps the maps around you in memory.
// Game state is read on the game's task thread (RunFrame); the menu and HUD draw from what it publishes.
namespace DropFinder {

// From CoopResync's per-frame game task
void RunFrame();

// The "Find an item" block in the F8 menu (between the window's Begin and End): opens the finder window, and
// stops tracking
void DrawMenuSection(float scale);

// The finder window, while the F8 menu is open and the finder is on (after the menu's End); it first opens at
// nextTo
void DrawWindow(float scale, ImVec2 nextTo);
// Whether the finder is on, and how wide it opens (so the menu can make room)
bool WindowOpen();
float WindowWidth(float scale);

// The arrow and distance to the tracked spot (every frame)
void DrawTracker(ImFont* font);

// What is being tracked ("" for nothing), and stop it (the native menu's Stop tracking)
std::string Tracking();
void StopTracking();

// Where the tracked spot is, on the world map's frame (game task thread; false when nothing is tracked)
bool TrackedSpot(World::WPos& at, std::string& label);

} // namespace DropFinder
