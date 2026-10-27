#pragma once

// Elden Ring co-op "resync" (game 2.7.2.0), in the F8 menu:
// - Resync players: repairs this game's copy of another player you can't see (revives a copy
//   left faded out by a death, or removes a badly built one so the game builds it again as on
//   join). Only your own view changes; whoever can't see their friend presses it.
// - Reload here: reloads the map where you stand, which rebuilds every other player's character
//   the way resting at a grace does.
// - Go to host (guests only): the same reload, at the host's position.
// Game state is only touched on the game's own task thread (a recurring FrameBegin task); the
// menu reads a snapshot and queues requests.
namespace CoopResync {

// Check the game code and hook the member-left handler. Elden Ring only, after MH_Initialize.
void Init();

// Wait for the game's task system and register our per-frame task. Blocks; call from the
// init thread after Init.
void StartTask();

// The "Co-op" block at the top of the F8 menu (between the window's Begin and End).
void DrawMenuSection(float scale);

} // namespace CoopResync
