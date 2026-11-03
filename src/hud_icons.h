#pragma once
#include <cstdint>

// Elden Ring: the status icons the HUD shows under the health bars, in their order on screen. The game builds them
// in a list of 42 slots (0x40 bytes each, the icon id as a float at +0x3c) that its icon-strip update (0x8d58f0,
// called only from the HUD update) gets in r8; we hook that update to see the list. On screen they sit in rows of 14,
// 42 px apart and 41 px between rows at 1080p (as JourneyHUD found), the first centred at x 155.5, its timer at y 146.
namespace HudIcons {

constexpr int kSlots = 42, kPerRow = 14;

// Hook the update (game task thread, once the world is up; does nothing after the first call)
void Install();

// The icons on screen now (render thread); 0 when the strip is hidden, not hooked or not updated lately
int Icons(int32_t (&ids)[kSlots]);

} // namespace HudIcons
