#pragma once
#include "buff_timers.h"
#include "imgui.h"

// Elden Ring: a panel for the enemy you're locked on to (under the minimap): name, health and posture with numbers,
// status buildup (poison, rot, bleed, blight, frost, sleep, madness) and its effects with the time left. All read
// live: PlayerIns+0x6b0 is the lock-on target's handle, found in WorldChrMan's character sets; health in its data
// module, posture in its super-armor module, buildup in its resist module, effects in its SpEffect list.
namespace Target {

// From CoopResync's per-frame game task
void RunFrame();

// Draw it (render thread); sheets are the status-icon textures the buff timers use
void Draw(ImFont* font, const BuffTimers::Sheets& sheets);

} // namespace Target
