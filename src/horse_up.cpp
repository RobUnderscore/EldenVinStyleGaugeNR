#include "horse_up.h"
#include "config.h"
#include "coop_resync.h"
#include "game_textures.h"
#include "world.h"
#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace HorseUp {

using namespace World;

static constexpr uintptr_t WORLD_CHR_MAN = 0x3d6d018;  // +0x1e508 the main player
static constexpr uintptr_t MAIN_PLAYER = 0x1e508;
// ChrIns+0x190 modules, +0xe8 CSChrRideModule (owner +0x8, ride node +0x10); the node (a CSPairAnimNode: owner
// +0x10) keeps the ride state at +0x50. Getting on with the whistle went 0 -> 1 -> 3 -> 5 (2026-11-02).
static constexpr uintptr_t MODULES = 0x190, RIDE_MODULE = 0xe8, RIDE_OWNER = 0x8, RIDE_NODE = 0x10, NODE_OWNER = 0x10,
                           RIDE_STATE = 0x50;
enum RideState { NONE = 0, GETTING_ON = 3, RIDING = 5, GETTING_OFF = 7 };
// Another player's song: full volume within kNear metres, then falling off with distance, silent from kSilent
static constexpr float kNear = 6.0f, kSilent = 120.0f;
static constexpr int kMaxVoices = 4;

// ---- the songs (HorseUp\songs.txt: a file, then the name shown) ----

struct Clip {
    std::string file, id, name;  // id: the file without .wav, what the co-op link sends
    std::once_flag once;
    std::vector<int16_t> samples;
    WAVEFORMATEX format{};
    bool ok = false;
};
static std::vector<std::unique_ptr<Clip>> s_Clips;  // fixed once read
static std::once_flag s_ClipsOnce;

static void ReadSongs() {
    std::call_once(s_ClipsOnce, [] {
        char path[MAX_PATH], line[256];
        GameTextures::PathNextToDll("HorseUp\\songs.txt", path, sizeof(path));
        FILE* f = fopen(path, "r");
        if (!f) return Log("HorseUp: %s is missing", path);
        while (fgets(line, sizeof(line), f)) {
            char file[96];
            int used = 0;
            if (line[0] == '#' || sscanf(line, " %95s %n", file, &used) != 1) continue;
            size_t n = strlen(file);
            if (n < 5 || _stricmp(file + n - 4, ".wav")) continue;
            auto c = std::make_unique<Clip>();
            c->file = file;
            c->id.assign(file, n - 4);
            c->name = line + used;
            while (!c->name.empty() && strchr(" \t\r\n", c->name.back())) c->name.pop_back();
            if (c->name.empty()) c->name = c->id;
            s_Clips.push_back(std::move(c));
        }
        fclose(f);
        Log("HorseUp: %d songs in %s", (int)s_Clips.size(), path);
    });
}

static Clip* FindClip(const std::string& id) {
    ReadSongs();
    for (auto& c : s_Clips)
        if (c->id == id) return c.get();
    return nullptr;
}

// A 16-bit PCM WAV, read once
static bool Load(Clip& c) {
    std::call_once(c.once, [&c] {
        char path[MAX_PATH];
        GameTextures::PathNextToDll(("HorseUp\\" + c.file).c_str(), path, sizeof(path));
        std::vector<uint8_t> b;
        if (FILE* f = fopen(path, "rb")) {
            uint8_t chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
            fclose(f);
        }
        if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
            return Log("HorseUp: %s is missing or not a WAV", path);
        // RIFF chunks: "fmt " (PCM format) and "data"
        bool fmt = false;
        for (size_t p = 12; p + 8 <= b.size();) {
            uint32_t size;
            memcpy(&size, &b[p + 4], 4);
            size_t avail = (std::min)((size_t)size, b.size() - p - 8);
            const uint8_t* d = &b[p + 8];
            if (!memcmp(&b[p], "fmt ", 4) && avail >= 16) {
                memcpy(&c.format.wFormatTag, d, 2);
                memcpy(&c.format.nChannels, d + 2, 2);
                memcpy(&c.format.nSamplesPerSec, d + 4, 4);
                memcpy(&c.format.wBitsPerSample, d + 14, 2);
                fmt = true;
            } else if (!memcmp(&b[p], "data", 4)) {
                c.samples.resize(avail / 2);
                memcpy(c.samples.data(), d, c.samples.size() * 2);
            }
            p += 8 + (size_t)size + (size & 1);
        }
        if (!fmt || c.format.wFormatTag != WAVE_FORMAT_PCM || c.format.wBitsPerSample != 16 || c.format.nChannels < 1 ||
            c.format.nChannels > 2 || c.format.nSamplesPerSec < 8000 || c.samples.size() < c.format.nChannels)
            return Log("HorseUp: %s isn't 16-bit PCM", path);
        c.ok = true;
        Log("HorseUp: %s, %.1f s", path, c.samples.size() / (double)(c.format.nSamplesPerSec * c.format.nChannels));
    });
    return c.ok;
}

// ---- settings ----

static std::atomic<bool> s_On{ true }, s_Others{ true };
static std::atomic<int> s_Volume{ 70 };
static std::string s_Song;  // a song's id, or "shuffle" / empty: a different one each time (under s_SettingsLock)
static std::mutex s_SettingsLock;
static std::once_flag s_SettingsOnce;

static void LoadSettings() {
    std::call_once(s_SettingsOnce, [] {
        char path[MAX_PATH], line[128];
        if (!Config::UserFile("HorseUp.ini", path, sizeof(path))) return;
        if (FILE* f = fopen(path, "r")) {
            while (fgets(line, sizeof(line), f)) {
                char key[64], value[64];
                if (sscanf(line, " %63[a-z_] = %63s", key, value) != 2) continue;
                int v = atoi(value);
                if (!strcmp(key, "play")) s_On = v != 0;
                else if (!strcmp(key, "others")) s_Others = v != 0;
                else if (!strcmp(key, "volume")) s_Volume = (std::clamp)(v, 0, 100);
                else if (!strcmp(key, "song")) s_Song = value;
            }
            fclose(f);
        }
        Log("HorseUp: %s, others %s, volume %d%%, song %s (%s)", s_On ? "on" : "off", s_Others ? "on" : "off", s_Volume.load(),
            s_Song.empty() ? "shuffle" : s_Song.c_str(), path);
    });
}

static void SaveSettings() {
    char path[MAX_PATH];
    if (!Config::UserFile("HorseUp.ini", path, sizeof(path))) return;
    FILE* f = fopen(path, "w");
    if (!f) return Log("HorseUp: couldn't save %s", path);
    std::string song;
    {
        std::lock_guard<std::mutex> lock(s_SettingsLock);
        song = s_Song;
    }
    fprintf(f, "# Horse Up when you get on Torrent (Esc > System > Game Options > Vin-Swag > Horse Up)\n");
    fprintf(f, "# play: 1 on, 0 off; others: hear the other players' (1, 0); volume: 0..100 (percent);\n");
    fprintf(f, "# song: a file name in HorseUp\\songs.txt without .wav, or shuffle (the default)\n");
    fprintf(f, "play = %d\nothers = %d\nvolume = %d\n", s_On ? 1 : 0, s_Others ? 1 : 0, s_Volume.load());
    if (!song.empty()) fprintf(f, "song = %s\n", song.c_str());
    fclose(f);
}

bool Enabled() { LoadSettings(); return s_On; }
void SetEnabled(bool on) { LoadSettings(); s_On = on; SaveSettings(); }
bool Others() { LoadSettings(); return s_Others; }
void SetOthers(bool on) { LoadSettings(); s_Others = on; SaveSettings(); }
int Volume() { LoadSettings(); return s_Volume; }
void SetVolume(int percent) { LoadSettings(); s_Volume = (std::clamp)(percent, 0, 100); SaveSettings(); }

std::vector<std::string> SongNames() {
    ReadSongs();
    std::vector<std::string> out;
    for (auto& c : s_Clips) out.push_back(c->name);
    return out;
}

int Song() {
    LoadSettings();
    ReadSongs();
    std::lock_guard<std::mutex> lock(s_SettingsLock);
    for (size_t i = 0; i < s_Clips.size(); i++)
        if (s_Clips[i]->id == s_Song) return (int)i;
    return -1;  // shuffle (the default)
}

void SetSong(int index) {
    LoadSettings();
    ReadSongs();
    {
        std::lock_guard<std::mutex> lock(s_SettingsLock);
        s_Song = index < 0 || index >= (int)s_Clips.size() ? std::string("shuffle") : s_Clips[index]->id;
    }
    SaveSettings();
}

// The song to play now: the one chosen, or for shuffle any other than last time's
static Clip* Pick() {
    static std::mutex lock;
    static std::mt19937 rng{ (uint32_t)GetTickCount64() };
    static int last = -1;
    std::lock_guard<std::mutex> hold(lock);
    int i = Song();
    if (s_Clips.empty()) return nullptr;
    if (i < 0) {
        int n = (int)s_Clips.size();
        i = n > 1 ? (int)(rng() % (n - 1)) : 0;
        if (n > 1 && i >= last && last >= 0) i++;
        last = i;
    }
    return s_Clips[i].get();
}

// ---- voices ----

struct Voice {
    Clip* clip;
    bool spatial;   // another player's: mono, aimed by the task thread
    uint64_t from;  // their Steam ID
    std::atomic<float> left{ 0 }, right{ 0 };  // gains now (playback ramps to them a buffer at a time)
    std::atomic<bool> done{ false };
};
static std::mutex s_VoiceLock;
static std::vector<std::shared_ptr<Voice>> s_Voices;  // under s_VoiceLock

static int16_t Sample(float v) { return (int16_t)(std::clamp)(v, -32768.0f, 32767.0f); }

// A clip through waveOut in 50 ms buffers, each at the voice's gains of the moment (stereo out)
static DWORD WINAPI PlayThread(LPVOID param) {
    std::shared_ptr<Voice> v = *static_cast<std::shared_ptr<Voice>*>(param);
    delete static_cast<std::shared_ptr<Voice>*>(param);
    const Clip& c = *v->clip;
    const int ch = c.format.nChannels;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = c.format.nSamplesPerSec;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
    const size_t frames = c.samples.size() / ch, chunk = fmt.nSamplesPerSec / 20;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    MMRESULT r = ev ? waveOutOpen(&out, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT) : MMSYSERR_ERROR;
    if (r != MMSYSERR_NOERROR) {
        Log("HorseUp: no sound device (waveOutOpen %u)", (unsigned)r);
        if (ev) CloseHandle(ev);
        v->done = true;
        return 0;
    }
    constexpr int kBuffers = 4;
    std::vector<int16_t> buf[kBuffers];
    WAVEHDR hdr[kBuffers] = {};
    bool queued[kBuffers] = {};
    size_t pos = 0;
    float gl = v->left, gr = v->right;
    auto fill = [&](int b) {
        if (pos >= frames) return false;
        size_t n = (std::min)(chunk, frames - pos);
        float tl = v->left, tr = v->right;
        buf[b].resize(n * 2);
        for (size_t i = 0; i < n; i++) {
            float t = (float)(i + 1) / n, l = gl + (tl - gl) * t, rr = gr + (tr - gr) * t;
            const int16_t* in = &c.samples[(pos + i) * ch];
            float a = in[0], z = ch > 1 ? in[1] : in[0];
            if (v->spatial) a = z = (a + z) * 0.5f;
            buf[b][i * 2] = Sample(a * l);
            buf[b][i * 2 + 1] = Sample(z * rr);
        }
        gl = tl, gr = tr;
        pos += n;
        hdr[b] = {};
        hdr[b].lpData = reinterpret_cast<LPSTR>(buf[b].data());
        hdr[b].dwBufferLength = (DWORD)(n * 4);
        if (waveOutPrepareHeader(out, &hdr[b], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        if (waveOutWrite(out, &hdr[b], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) return true;
        waveOutUnprepareHeader(out, &hdr[b], sizeof(WAVEHDR));
        return false;
    };
    for (int b = 0; b < kBuffers; b++) queued[b] = fill(b);
    ULONGLONG deadline = GetTickCount64() + frames * 1000 / fmt.nSamplesPerSec + 3000;
    for (;;) {
        bool any = false;
        for (int b = 0; b < kBuffers; b++) {
            if (queued[b] && (hdr[b].dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(out, &hdr[b], sizeof(WAVEHDR));
                queued[b] = fill(b);
            }
            any |= queued[b];
        }
        if (!any || GetTickCount64() > deadline) break;
        WaitForSingleObject(ev, 100);
    }
    waveOutReset(out);
    for (int b = 0; b < kBuffers; b++)
        if (queued[b]) waveOutUnprepareHeader(out, &hdr[b], sizeof(WAVEHDR));
    waveOutClose(out);
    CloseHandle(ev);
    v->done = true;
    return 0;
}

// Start a clip: ours (not over one of ours still playing), or another player's
static bool Start(Clip* clip, bool spatial, uint64_t from) {
    if (!clip || !Load(*clip) || Volume() == 0) return false;
    std::lock_guard<std::mutex> lock(s_VoiceLock);
    s_Voices.erase(std::remove_if(s_Voices.begin(), s_Voices.end(), [](const std::shared_ptr<Voice>& v) { return v->done.load(); }),
                   s_Voices.end());
    if ((int)s_Voices.size() >= kMaxVoices) return false;
    for (const auto& v : s_Voices)
        if (v->spatial == spatial && v->from == from) return false;
    auto v = std::make_shared<Voice>();
    v->clip = clip, v->spatial = spatial, v->from = from;
    if (!spatial) v->left = v->right = Volume() / 100.0f;
    auto* arg = new std::shared_ptr<Voice>(v);
    HANDLE th = CreateThread(nullptr, 0, PlayThread, arg, 0, nullptr);
    if (!th) {
        delete arg;
        return false;
    }
    CloseHandle(th);
    s_Voices.push_back(v);
    return true;
}

void Play() {
    LoadSettings();
    ReadSongs();
    Start(Pick(), false, 0);
}

void Heard(uint64_t steamId, const char* song) {
    Clip* clip = FindClip(song);
    Log("HorseUp: %llX got on Torrent with %s%s", (unsigned long long)steamId, song,
        !clip ? " (not in our songs.txt)" : !Others() ? " (others' songs off)" : "");
    if (clip && Others()) Start(clip, true, steamId);
}

// Gains for a song played at `at`, heard at `me` with the camera looking along `heading` (degrees from north):
// equal-power panning by the angle off the camera, a little quieter behind, and quieter with distance
static void Gains(const WPos& me, float heading, const WPos& at, float& left, float& right) {
    float dx = at.x - me.x, dy = at.y - me.y, dz = at.z - me.z;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    left = right = 0;
    if (!std::isfinite(d) || d >= kSilent) return;
    float g = (d <= kNear ? 1.0f : kNear / d) * Volume() / 100.0f;
    if (d > kSilent * 0.75f) g *= (kSilent - d) / (kSilent * 0.25f);
    float pan = 0;
    if (dx * dx + dz * dz > 0.25f) {
        float off = (atan2f(dx, dz) * 57.29578f - heading) * 0.01745329f;
        pan = sinf(off);
        if (cosf(off) < 0) g *= 1.0f + 0.3f * cosf(off);  // up to 30% quieter straight behind
    }
    float a = (pan + 1.0f) * 0.78539816f;
    left = g * 1.41421356f * cosf(a);
    right = g * 1.41421356f * sinf(a);
}

// Aim the other players' songs at where they are now (their overlay's live spot)
static void Aim() {
    std::vector<std::shared_ptr<Voice>> voices;
    {
        std::lock_guard<std::mutex> lock(s_VoiceLock);
        for (const auto& v : s_Voices)
            if (v->spatial && !v->done) voices.push_back(v);
    }
    if (voices.empty()) return;
    WPos me;
    float phys[3], heading = 0;
    bool known = Ready() && Player(me, phys) && CameraHeading(phys, heading);
    std::vector<CoopResync::Partner> partners = known ? CoopResync::Partners() : std::vector<CoopResync::Partner>{};
    for (const auto& v : voices) {
        float l = 0, r = 0;
        for (const CoopResync::Partner& p : partners) {
            WPos at;
            if (p.steamId == v->from && ToWorld((uint32_t)p.block, p.x, p.y, p.z, at) && at.space == me.space) Gains(me, heading, at, l, r);
        }
        v->left = l, v->right = r;
    }
}

// ---- getting on ----

static uintptr_t s_Player = 0;
static int s_State = -1;  // the ride state last seen (-1: not known yet)
static ULONGLONG s_SeenAt = 0, s_PlayedAt = 0;
static bool s_Warned = false;

void RunFrame() {
    if (!Base()) return;
    Aim();
    uintptr_t wcm = Global(WORLD_CHR_MAN);
    uintptr_t player = wcm ? Ptr(wcm + MAIN_PLAYER) : 0;
    uintptr_t mods = player ? Ptr(player + MODULES) : 0;
    uintptr_t ride = mods ? Ptr(mods + RIDE_MODULE) : 0;
    uintptr_t node = ride ? Ptr(ride + RIDE_NODE) : 0;
    // both point back at the player when the layout is the one expected
    if (!node || Ptr(ride + RIDE_OWNER) != player || Ptr(node + NODE_OWNER) != player) {
        if (ride && !s_Warned) {
            s_Warned = true;
            Log("HorseUp: the ride module isn't laid out as expected (player %p, module %p, node %p); no Horse Up", (void*)player,
                (void*)ride, (void*)node);
        }
        s_Player = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (player != s_Player) {  // a new character (after a load): learn its state before going by changes
        s_Player = player;
        s_State = -1;
        s_SeenAt = now;
    }
    int state = (int)Rd<uint32_t>(node + RIDE_STATE);
    if (state == s_State) return;
    if (s_State >= 0) Log("HorseUp: ride state %d -> %d", s_State, state);
    // getting on (or straight to riding), from any state but those two; not just after a load, nor again within 8 s
    bool gettingOn = (state == GETTING_ON || state == RIDING) && s_State != GETTING_ON && s_State != RIDING;
    if (gettingOn && now - s_SeenAt > 3000 && now - s_PlayedAt > 8000 && Enabled()) {
        s_PlayedAt = now;
        ReadSongs();
        if (Clip* clip = Pick()) {
            Start(clip, false, 0);
            CoopResync::SendHorseUp(clip->id);  // the others hear it from where we are
        }
    }
    s_State = state;
}

} // namespace HorseUp
