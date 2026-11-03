#pragma once
#include "imgui.h"
#include <cstdint>
#include <string>
#include <vector>

// Elden Ring co-op "resync" (game 2.7.2.0), in the F8 menu:
// - Resync players: repairs this game's copy of another player you can't see (revives a copy
//   left faded out by a death, or removes a badly built one so the game builds it again as on
//   join). Only your own view changes; whoever can't see their friend presses it.
// - Auto-resync (on by default): the same repairs every 10 s by themselves, and a rebuild as soon
//   as a copy stays far from where that player's own game says they are; only for a player whose
//   own overlay says over the link that they're alive, nearby and not loading.
// - Reload here: reloads the map where you stand, which rebuilds every other player's character
//   the way resting at a grace does.
// - Go to host (guests only): the same reload, at the host's position.
// - Invasions: the host summons AI Tarnished that every player sees and fights (spirit-ash buddies), with the
//   HP the game's own NPC invaders have in the host's area, sometimes with 1-3 summons (more Tarnished or,
//   with summon_ashes on, hostile spirit ashes). Every game also keeps the summons another player brought
//   when Seamless's stale one-shot dismissal would drop them on arrival.
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

// "Invaded by ...!" for a few seconds after an invader comes (every player's game).
void DrawBanner(ImFont* font);

// For the native menu (native_menu.h): the requests the F8 buttons make. Request starts one and returns "", or
// returns why it can't, for the player
enum class Action { Resync, Reload, GoToHost, ClearBoss, SummonInvader };
std::string Request(Action action);
// The F8 switches (kept in Coop.ini)
bool AutoResync();
void SetAutoResync(bool on);
bool PositionCheck();
void SetPositionCheck(bool on);
// The newest status message and its number (0 before the first; changes with every new message)
uint32_t LatestMessage(std::string& text);

// The other players in the session, where their own overlay last said they are (its live spot: block and position
// as PlayerIns+0x6d0/+0x6c0 keep them, sent twice a second); only fresh ones. For the minimap; game task thread only
struct Partner {
    std::string name;
    bool isHost;
    int32_t block;
    float x, y, z, yaw;
    uint64_t steamId = 0;
};
std::vector<Partner> Partners();

// Horse Up: tell the other players' overlays we got on Torrent with this song (its id in HorseUp\songs.txt); theirs
// call HorseUp::Heard. Game task thread
void SendHorseUp(const std::string& song);

// A guest in someone else's world (Seamless): the event flags the game reads are then the host's world's, with the
// guest's own changes this session. Game task thread
bool InHostsWorld();

} // namespace CoopResync
