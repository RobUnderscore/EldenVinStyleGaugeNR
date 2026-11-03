#pragma once
#include <cstdint>
#include <string>
#include <vector>

// "Horse Up": a few seconds of a horse song, faded out, when you get on Torrent, and the other players' songs when
// they get on theirs, from where they are. The clips and their names are in HorseUp\songs.txt next to the DLL, 16-bit
// PCM WAVs cut by er-sounds/horse-up/cut.sh; by default each ride picks one at random (not last time's). Played
// through waveOut, a voice per clip on a thread of its own, at the volume set here.
// The ride state is the main player's CSChrRideModule ([[ChrIns+0x190]+0xe8]) ride node (+0x10): its state at +0x50
// is 0 none, 1 (the whistle, before getting on), 3 getting on, 5 riding, 7 getting off (struct layout from the
// eldenring crate, fromsoftware-rs). Getting on, the song's id goes to the others over CoopResync's link; theirs
// play panned by where that player is (their live spot, twice a second) against our camera, quieter with distance
// and silent from 120 m.
namespace HorseUp {

// From CoopResync's per-frame game task
void RunFrame();
// From CoopResync's link (game task): another player got on Torrent with this song
void Heard(uint64_t steamId, const char* song);

// Settings, kept in HorseUp.ini in the user config folder (the Vin-Swag page in Game Options changes them)
bool Enabled();  // play yours
void SetEnabled(bool on);
bool Others();   // play the other players'
void SetOthers(bool on);
int Volume();    // percent, 0..100
void SetVolume(int percent);
// The songs' names, in songs.txt's order; the one chosen is an index into them, or -1 (the default) for a different
// one each time
std::vector<std::string> SongNames();
int Song();
void SetSong(int index);

// Play the chosen song now (the menu's "Hear it")
void Play();

} // namespace HorseUp
