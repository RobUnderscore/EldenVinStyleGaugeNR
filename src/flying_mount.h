#pragma once

// Flying Mount (testing, VIZZO's Nexus mod 6628 made switchable): Torrent flies only while this is on, and it is off
// at every start. On, the main player keeps SpEffect 100910, the marker that the Lua module (elden-vins
// action/script/modules/FlyingMount.lua) and the event script (common.emevd 12110268-9) go by: it lasts 2 s and is
// put back twice a second through the game's own apply-SpEffect routine (0x3e8dc0(chr, id, dont_sync = 1),
// er-coop/docs/spec/party.md), so only this machine has it. Off, it runs out. The game files: er-flying-mount/.
namespace FlyingMount {

// From CoopResync's per-frame game task
void RunFrame();

bool Enabled();
void SetEnabled(bool on);

// The F8 window's section
void DrawMenuSection(float scale);

} // namespace FlyingMount
