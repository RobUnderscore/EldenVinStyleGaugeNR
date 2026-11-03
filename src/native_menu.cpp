#include "native_menu.h"
#include "convenience_menu.h"
#include "coop_resync.h"
#include "drop_finder.h"
#include "horse_up.h"
#include "minimap.h"
#include <ernativeui/ERNativeUI.hpp>
#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace NativeMenu {

static erui::Registration s_Reg;
static std::atomic<bool> s_Active{ false };
// a native button was used lately: show the co-op messages it leads to as native notices too
static std::atomic<ULONGLONG> s_AskedAt{ 0 };
static std::vector<ConvenienceMenu::Gift> s_Gifts;  // fixed after registration: the buttons point into it
static bool s_NumericSlider = false, s_InlineChoice = false, s_PopupChoice = false;  // what this ERNativeUI offers

static std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// A native notice at the bottom of the screen
static void Tell(const std::string& text) {
    if (text.empty() || !s_Active) return;
    try {
        ERUI_Result r = s_Reg.alert(Wide(text), erui::AlertOptions{ erui::AlertButtons::ok, erui::AlertPlacement::bottom });
        if (r != ERUI_OK) Log("NativeMenu: notice not shown (%u): %s", (unsigned)r, text.c_str());
    } catch (...) {
    }
}

static void Run(CoopResync::Action a) {
    s_AskedAt = GetTickCount64();
    std::string why = CoopResync::Request(a);
    if (!why.empty()) Tell(why);
}

// Buttons (the game's UI thread) and alert replies (ERNativeUI's worker): only queue work and post notices
static void ERUI_CALL OnConfirmed(void* user, ERUI_Result result, erui::AlertResponse response) noexcept {
    if (result == ERUI_OK && response == erui::AlertResponse::primary)
        Run(static_cast<CoopResync::Action>(reinterpret_cast<uintptr_t>(user)));
}

static void Confirm(CoopResync::Action a, const wchar_t* question) {
    try {
        ERUI_Result r = s_Reg.alert(question, erui::AlertOptions{ erui::AlertButtons::yes_no, erui::AlertPlacement::center },
                                    &OnConfirmed, reinterpret_cast<void*>(static_cast<uintptr_t>(a)));
        if (r != ERUI_OK) Run(a);  // no dialog: do it, as F8's second click would
    } catch (...) {
    }
}

static void ERUI_CALL OnResync(void*) noexcept { Run(CoopResync::Action::Resync); }
static void ERUI_CALL OnReload(void*) noexcept {
    Confirm(CoopResync::Action::Reload, L"Reload here? A loading screen at your spot rebuilds every player, like resting at a grace (enemies respawn too).");
}
static void ERUI_CALL OnGoToHost(void*) noexcept {
    Confirm(CoopResync::Action::GoToHost, L"Go to the host? A loading screen, then you're next to them.");
}
static void ERUI_CALL OnClearBoss(void*) noexcept { Run(CoopResync::Action::ClearBoss); }
static void ERUI_CALL OnInvader(void*) noexcept { Run(CoopResync::Action::SummonInvader); }
static void ERUI_CALL OnAutoResync(void*, std::uint8_t v) noexcept { CoopResync::SetAutoResync(v != 0); }
static void ERUI_CALL OnPositionCheck(void*, std::uint8_t v) noexcept { CoopResync::SetPositionCheck(v != 0); }

static void ERUI_CALL OnMinimap(void*, std::uint8_t v) noexcept { Minimap::SetEnabled(v != 0); }
static void OnMinimapOpacity(const erui::NumericSliderChange& c) noexcept { Minimap::SetOpacity((int)(c.value + 0.5)); }
static void ERUI_CALL OnMinimapOpacityByte(void*, std::uint8_t v) noexcept { Minimap::SetOpacity(v); }
static void ERUI_CALL OnMinimapBoss(void*, std::uint8_t v) noexcept { Minimap::SetBossFights(v); }
static void ERUI_CALL OnMinimapBossFade(void*, std::uint8_t v) noexcept {
    Minimap::SetBossFights(v ? Minimap::BOSS_FADE : Minimap::BOSS_KEEP);
}

static void ERUI_CALL OnHorseUp(void*, std::uint8_t v) noexcept { HorseUp::SetEnabled(v != 0); }
static void ERUI_CALL OnHorseUpOthers(void*, std::uint8_t v) noexcept { HorseUp::SetOthers(v != 0); }
static void OnHorseUpVolume(const erui::NumericSliderChange& c) noexcept { HorseUp::SetVolume((int)(c.value + 0.5)); }
static void ERUI_CALL OnHorseUpVolumeByte(void*, std::uint8_t v) noexcept { HorseUp::SetVolume(v); }
static void ERUI_CALL OnHorseUpHear(void*) noexcept { HorseUp::Play(); }
// the choice's values: the songs, then shuffle (the last) -> HorseUp::Song() -1
static std::vector<std::wstring> s_SongNames;
static std::vector<std::wstring_view> s_SongViews;
static void ERUI_CALL OnHorseUpSong(void*, std::uint8_t v) noexcept {
    HorseUp::SetSong(v + 1 < s_SongViews.size() ? (int)v : -1);
}
static void ERUI_CALL OnHorseUpPick(void* user) noexcept {  // without inline choices: a button per song
    HorseUp::SetSong((int)reinterpret_cast<intptr_t>(user));
    HorseUp::Play();
}

static void AddHorseUp(erui::Page& page) {
    erui::Page horse = page.add_submenu(L"Horse Up", L"A few seconds of a horse song when you get on Torrent: which one, how loud, and the other players' songs.",
                                        L"Horse Up");
    if (!horse.valid()) return;
    horse.add_toggle(L"Play when you get on Torrent", L"A few seconds of your song, fading out (not again within 8 s). The other players hear it from where you are. Remembered.",
                     HorseUp::Enabled() ? 1 : 0, &OnHorseUp);
    std::vector<std::string> songs = HorseUp::SongNames();
    if (!songs.empty()) {
        s_SongNames.clear();
        for (const std::string& name : songs) s_SongNames.push_back(Wide(name));
        s_SongNames.push_back(L"Shuffle");
        s_SongViews.assign(s_SongNames.begin(), s_SongNames.end());
        int chosen = HorseUp::Song();
        const wchar_t* songHelp = L"Yours when you get on Torrent, or Shuffle (the default) for a different one each time. Remembered.";
        erui::ChoiceOptions o{ s_SongViews.data(), s_SongViews.size(), (std::uint8_t)(chosen < 0 ? s_SongViews.size() - 1 : chosen) };
        if (s_PopupChoice) {  // a list: there are a lot of songs to step through one by one
            horse.add_popup_choice(L"Song", songHelp, o, &OnHorseUpSong);
        } else if (s_InlineChoice) {
            horse.add_inline_choice(L"Song", songHelp, o, &OnHorseUpSong);
        } else {
            erui::Page pick = horse.add_submenu(L"Song", L"Yours when you get on Torrent. Remembered.", L"Song");
            for (size_t i = 0; pick.valid() && i < s_SongViews.size(); i++)
                pick.add_button(s_SongNames[i], L"Use this one (and hear it).", &OnHorseUpPick,
                                reinterpret_cast<void*>((intptr_t)(i + 1 < s_SongViews.size() ? (int)i : -1)));
        }
    }
    horse.add_toggle(L"Hear the others' songs", L"When another player gets on Torrent, their song plays from where they are (quieter the further off, silent from 120 m). Remembered.",
                     HorseUp::Others() ? 1 : 0, &OnHorseUpOthers);
    const wchar_t* volumeHelp = L"How loud the songs play (yours and the others'). Remembered.";
    if (s_NumericSlider) {
        erui::NumericSliderOptions o{};
        o.minimum = 0, o.maximum = 100, o.step = 5, o.initial_value = HorseUp::Volume();
        o.decimal_places = 0u;
        o.suffix = L"%";
        horse.add_slider<&OnHorseUpVolume>(L"Volume", volumeHelp, o);
    } else {
        erui::SliderOptions o{};
        o.minimum = 0, o.maximum = 100, o.step = 5, o.initial_value = (std::uint8_t)HorseUp::Volume();
        horse.add_slider(L"Volume", volumeHelp, o, &OnHorseUpVolumeByte);
    }
    horse.add_button(L"Hear it", L"Plays your song now at this volume (Shuffle: one of them).", &OnHorseUpHear);
}

static void AddMinimap(erui::Page& page) {
    erui::Page map = page.add_submenu(L"Minimap", L"The map in the top right corner: on or off, how see-through, and boss fights.", L"Minimap");
    if (!map.valid()) return;
    map.add_toggle(L"Show minimap", L"The game's own map around you, with your co-op partners, graces, beacons and what you track. Remembered.",
                   Minimap::Enabled() ? 1 : 0, &OnMinimap);
    const wchar_t* opacityHelp = L"How solid the minimap is (100% = fully). Remembered.";
    if (s_NumericSlider) {
        erui::NumericSliderOptions o{};
        o.minimum = 10, o.maximum = 100, o.step = 5, o.initial_value = Minimap::Opacity();
        o.decimal_places = 0u;
        o.suffix = L"%";
        map.add_slider<&OnMinimapOpacity>(L"Opacity", opacityHelp, o);
    } else {
        erui::SliderOptions o{};
        o.minimum = 10, o.maximum = 100, o.step = 5, o.initial_value = (std::uint8_t)Minimap::Opacity();
        map.add_slider(L"Opacity", opacityHelp, o, &OnMinimapOpacityByte);
    }
    if (s_InlineChoice) {
        static const std::wstring_view kBoss[] = { L"Keep it", L"Fade it", L"Hide it" };
        erui::ChoiceOptions o{ kBoss, 3, (std::uint8_t)Minimap::BossFights() };
        map.add_inline_choice(L"In boss fights", L"While a boss's health bar is up: keep the minimap, fade it to a quarter, or hide it. Remembered.",
                              o, &OnMinimapBoss);
    } else {
        map.add_toggle(L"Fade in boss fights", L"While a boss's health bar is up, the minimap fades to a quarter. Remembered.",
                       Minimap::BossFights() != Minimap::BOSS_KEEP ? 1 : 0, &OnMinimapBossFade);
    }
}

static void ERUI_CALL OnStopTracking(void*) noexcept {
    try {
        std::string what = DropFinder::Tracking();
        DropFinder::StopTracking();
        Tell(what.empty() ? "Nothing is being tracked (F8 > Find an item... > Track)" : "Stopped tracking " + what);
    } catch (...) {
    }
}

static void ERUI_CALL OnGift(void* user) noexcept {
    try {
        Tell(ConvenienceMenu::Receive(*static_cast<const ConvenienceMenu::Gift*>(user)));
    } catch (...) {
    }
}

// A row in System > Game Options opening our page. Not a custom System tab: in this pack ERNativeUI 1.3's tabs
// (built on the Graphics tab) found an extra tab record when the menu opened, gave up, and left Graphics blank
// (2026-11-01)
static void Build(erui::Menu& menu) {
    erui::Page page = menu.root().add_submenu(L"Vin-Swag", L"Co-op tools, switches and items from the Vin-Swag pack.", L"Vin-Swag");
    if (!page.valid()) return;

    page.add_button(L"Resync players", L"Can't see a friend? Repairs your game's copy of them; only your own view changes.", &OnResync);
    page.add_button(L"Reload here", L"A loading screen where you stand rebuilds every player, like resting at a grace (enemies respawn too).", &OnReload);
    page.add_button(L"Go to host", L"Guests: a loading screen, then you're next to the host (not with an invader on you, nor into a boss fight). Going to a guest is in F8.", &OnGoToHost);
    page.add_button(L"Clear boss fight flag", L"Your game thinks a boss fight is on with no boss bar (graces shut, Reload refused)? This turns it off.", &OnClearBoss);
    page.add_toggle(L"Auto-resync", L"Every 10 s, repairs a player you can't see, the way Resync players does: only when their game says they're alive, nearby and not loading. Remembered.",
                    CoopResync::AutoResync() ? 1 : 0, &OnAutoResync);
    page.add_toggle(L"Fix out-of-place players", L"With Auto-resync: also rebuild a player's character that stays more than 40 m from where their own game says they are. Remembered.",
                    CoopResync::PositionCheck() ? 1 : 0, &OnPositionCheck);
    page.add_button(L"Summon invader", L"Host or solo: an AI Tarnished invades now (they also come on their own, about once an hour). Invasions.ini sets the rest.", &OnInvader);
    page.add_button(L"Stop tracking", L"Removes the Find an item arrow (F8 > Find an item... > Track).", &OnStopTracking);
    AddMinimap(page);
    AddHorseUp(page);

    // ConvenienceMenu.txt, a page per section
    std::string section = "\x01";
    erui::Page items;
    for (const ConvenienceMenu::Gift& g : s_Gifts) {
        if (g.section != section) {
            section = g.section;
            std::wstring title = section.empty() ? std::wstring(L"Items") : Wide(section);
            items = page.add_submenu(title, L"Items from ConvenienceMenu.txt (changes there show after a restart).", title);
        }
        if (!items.valid()) continue;
        std::wstring label = Wide(g.label);
        if (g.quantity > 1) label += L" x" + std::to_wstring(g.quantity);
        items.add_button(label, L"Put it in your inventory.", &OnGift, const_cast<ConvenienceMenu::Gift*>(&g));
    }
}

// Co-op results that follow a native request (resync done, why a Go to was refused...) as native notices
static void Messages() {
    std::string text;
    uint32_t seen = CoopResync::LatestMessage(text);
    for (;;) {
        Sleep(250);
        uint32_t serial = CoopResync::LatestMessage(text);
        if (serial == seen) continue;
        seen = serial;
        if (GetTickCount64() - s_AskedAt.load() < 20000) Tell(text);
    }
}

static DWORD WINAPI Connect(LPVOID self) {
    std::string error;
    ConvenienceMenu::ReadGifts(s_Gifts, error);
    if (!error.empty()) Log("NativeMenu: %s", error.c_str());
    try {
        erui::ConnectionResult connection = erui::connect();
        if (!connection) {
            Log("NativeMenu: no ERNativeUI (%s); everything stays in F8", Narrow(connection.error().message()).c_str());
            return 1;
        }
        s_NumericSlider = connection.value().supports(erui::Capability::numeric_slider);
        s_InlineChoice = connection.value().supports(erui::Capability::inline_choice);
        s_PopupChoice = connection.value().supports(erui::Capability::popup_choice);
        erui::ProviderOptions options{};
        options.provider_id = "vin-swag-overlay";
        options.display_name = L"Vin-Swag";
        options.owner_module = self;
        erui::RegistrationResult reg = connection.value().register_menu(options, [](erui::Menu& menu) { Build(menu); });
        if (!reg) {
            Log("NativeMenu: registration failed (%s); everything stays in F8", Narrow(reg.error().message()).c_str());
            return 1;
        }
        s_Reg = reg.value();
        s_Active = true;
        Log("NativeMenu: \"Vin-Swag\" page in Game Options registered (API %u, %zu items)", s_Reg.api_version(), s_Gifts.size());
    } catch (...) {
        Log("NativeMenu: exception while connecting; everything stays in F8");
        return 1;
    }
    Messages();
    return 0;
}

void Start() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Start), &self);
    if (HANDLE th = CreateThread(nullptr, 0, Connect, self, 0, nullptr)) CloseHandle(th);
}

bool Active() { return s_Active; }

} // namespace NativeMenu
