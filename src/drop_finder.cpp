#include "drop_finder.h"
#include "coop_resync.h"
#include "game_textures.h"
#include "item_icons.h"
#include "world.h"
#include "imgui_internal.h"
#include "memory.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern void Log(const char* fmt, ...);

namespace DropFinder {

using namespace World;

// Row layouts (from the paramdefs; row sizes checked against the regulation's files)
namespace ItemLot {  // ItemLotParam_map / _enemy, 0x98
constexpr int ID = 0x00, CATEGORY = 0x20, BASE_POINT = 0x40, SLOT_FLAG = 0x60, FLAG = 0x80, NUM = 0x8a;
}
namespace Shop {  // ShopLineupParam, 0x34
constexpr int EQUIP_ID = 0x00, VALUE = 0x04, RELEASE_FLAG = 0x10, SELL_QUANTITY = 0x14, EQUIP_TYPE = 0x17, COST_TYPE = 0x18;
}
namespace Npc {  // NpcParam, 0x2e0
constexpr int NAME_ID = 0x0c, LOT_ENEMY = 0x30, LOT_MAP = 0x34;
}
namespace Goods {  // EquipParamGoods, 0xb0
constexpr int ICON = 0x30, MAX_NUM = 0x3a, TYPE = 0x3e;  // u16, s16, u8
}
// The icon id (u16) in each item table: weapon, armor (the male one), talisman, ash of war
constexpr int WEAPON_ICON = 0xbe, PROTECTOR_ICON = 0xa6, ACCESSORY_ICON = 0x26, GEM_ICON = 0x04;

// Item categories as item lots number them (shops: 0 weapon, 1 armor, 2 talisman, 3 goods, 4 ash of war, 5 custom)
enum Cat { GOODS = 1, WEAPON = 2, PROTECTOR = 3, ACCESSORY = 4, GEM = 5, CUSTOM_WEAPON = 6 };
static int64_t Key(int cat, int32_t id) { return (int64_t)cat << 32 | (uint32_t)id; }
// Weapons count by their base id (infusion and level stripped); a custom weapon by the weapon it's built on
static int32_t BaseWeapon(int32_t id) { return id - id % 10000; }

// ---- DropFinder.txt ----

struct Placed {
    uint32_t map;
    float x, y, z;
};
struct Chr {
    Placed at;
    int32_t entity, npc, talk;
};
struct Award {
    uint32_t map;
    std::vector<int32_t> entities;
};
struct ShopRange {
    int32_t talk, lo, hi;
};
struct HandIn {
    int32_t talk;
    uint32_t flag;  // set when the player hands it over
};
// A light-attack combo of a moveset (W lines): one round of the R1 chain, attacking again at the first chance
struct ComboHit {
    bool left;          // the left hand's weapon (powerstance)
    int32_t judge[4];   // behavior judge ids of attacks that overlap: the strongest counts
    int judges;
};
struct Combo {
    float seconds;
    int attacks;
    std::vector<ComboHit> hits;
};
enum Grip { ONE_HANDED = 1, TWO_HANDED = 2, POWERSTANCE = 3 };
struct Index {
    std::vector<Chr> chars;
    std::unordered_multimap<int32_t, int> byNpc, byTalk, byEntity;
    std::unordered_multimap<int32_t, Placed> pickups;  // item lot ->
    std::unordered_map<int32_t, Placed> assets;        // entity ->
    std::unordered_multimap<int32_t, Award> awards;    // item lot ->
    std::vector<ShopRange> shops;
    std::unordered_multimap<int32_t, int32_t> gifts;   // item lot -> talk
    std::unordered_multimap<int32_t, HandIn> handIns;  // goods id ->
    std::unordered_map<uint32_t, int32_t> handInFlags; // the flag -> goods id
    std::unordered_map<int, Combo> combos;             // moveset (wepmotionCategory) * 4 + Grip ->
    std::string built;
};
static Index s_Idx;
static std::atomic<int> s_IdxState{ 0 };  // 0 not read, 1 reading, 2 ready, 3 failed
static std::string s_IdxError;

static void ReadIndex() {
    char path[MAX_PATH];
    GameTextures::PathNextToDll("DropFinder.txt", path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        s_IdxError = "DropFinder.txt is missing next to the DLL";
        s_IdxState = 3;
        return;
    }
    Index idx;
    char line[1024];
    uint32_t map = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line + 2;
        switch (line[0]) {
        case '#':
            if (idx.built.empty()) {
                const char* b = strstr(line, "built ");
                if (b) idx.built.assign(b + 6, strcspn(b + 6, " "));
            }
            break;
        case 'M': map = strtoul(p, nullptr, 16); break;
        case 'C': {
            Chr c{};
            c.at.map = map;
            if (sscanf(p, "%d %d %d %f %f %f", &c.entity, &c.npc, &c.talk, &c.at.x, &c.at.y, &c.at.z) != 6) break;
            int i = (int)idx.chars.size();
            idx.chars.push_back(c);
            idx.byNpc.emplace(c.npc, i);
            if (c.talk > 0) idx.byTalk.emplace(c.talk, i);
            if (c.entity > 0) idx.byEntity.emplace(c.entity, i);
            break;
        }
        case 'T': {
            int32_t lot;
            Placed at{ map };
            if (sscanf(p, "%d %f %f %f", &lot, &at.x, &at.y, &at.z) == 4) idx.pickups.emplace(lot, at);
            break;
        }
        case 'P': {
            int32_t ent;
            Placed at{ map };
            if (sscanf(p, "%d %f %f %f", &ent, &at.x, &at.y, &at.z) == 4) idx.assets.emplace(ent, at);
            break;
        }
        case 'A': {
            int32_t lot = strtol(p, &p, 10);
            Award a{ map, {} };
            char* e = strstr(p, "e:");
            if (e) {
                e += 2;
                while (*e >= '0' && *e <= '9') {
                    a.entities.push_back(strtol(e, &e, 10));
                    if (*e == ',') e++;
                }
            }
            idx.awards.emplace(lot, a);
            break;
        }
        case 'S': {
            ShopRange s;
            // ids under 100000 are the shared talk functions, not anyone's own script
            if (sscanf(p, "%d %d %d", &s.talk, &s.lo, &s.hi) == 3 && s.lo >= 100000 && s.talk >= 100000) idx.shops.push_back(s);
            break;
        }
        case 'G': {
            int32_t talk, lot;
            if (sscanf(p, "%d %d", &talk, &lot) == 2 && talk >= 100000) idx.gifts.emplace(lot, talk);
            break;
        }
        case 'H': {  // goods + i handed over sets flag + i
            int32_t talk, goods, n;
            uint32_t flag;
            if (sscanf(p, "%d %d %u %d", &talk, &goods, &flag, &n) != 4 || talk < 100000) break;
            for (int i = 0; i < n && i < 200; i++) {
                idx.handIns.emplace(goods + i, HandIn{ talk, flag + i });
                idx.handInFlags.emplace(flag + i, goods + i);
            }
            break;
        }
        case 'W': {  // moveset grip seconds attacks, then hits: R or L, judge ids with / between
            int moveset, grip, used = 0;
            Combo c{};
            if (sscanf(p, "%d %d %f %d%n", &moveset, &grip, &c.seconds, &c.attacks, &used) != 4 || c.seconds <= 0) break;
            for (char* h = p + used;;) {
                while (*h == ' ') h++;
                if (*h != 'R' && *h != 'L') break;
                ComboHit hit{ *h++ == 'L', {}, 0 };
                for (;;) {
                    int32_t judge = strtol(h, &h, 10);
                    if (hit.judges < 4) hit.judge[hit.judges++] = judge;
                    if (*h != '/') break;
                    h++;
                }
                c.hits.push_back(hit);
            }
            if (!c.hits.empty()) idx.combos[moveset * 4 + grip] = std::move(c);
            break;
        }
        }
    }
    fclose(f);
    Log("DropFinder: DropFinder.txt (built %s): %d characters, %d pickups, %d awards, %d shop ranges, %d gifts, %d hand-ins, %d combos",
        idx.built.c_str(), (int)idx.chars.size(), (int)idx.pickups.size(), (int)idx.awards.size(), (int)idx.shops.size(),
        (int)idx.gifts.size(), (int)idx.handIns.size(), (int)idx.combos.size());
    s_Idx = std::move(idx);
    s_IdxState = 2;
}

// ---- item names (built on the game thread, a slice per frame) ----

// What the finder's kind filter sorts items into
enum Group { G_WEAPON, G_ARMOR, G_TALISMAN, G_SPELL, G_ASH, G_SPIRIT, G_KEY, G_ITEM, G_COUNT };
static const char* const kGroupNames[G_COUNT] = { "Weapons", "Armor", "Talismans", "Spells", "Ashes of War", "Spirit ashes",
                                                  "Key items", "Other items" };

struct Item {
    std::string name, lower;
    const char* kind;  // "Weapon", "Key item", ...
    int group;
    int icon;                   // the game's icon id (0 for none)
    std::vector<int64_t> keys;  // every id with this name and kind
    std::vector<int8_t> levels; // with keys: the upgrade level that id is ("Black Knife Tiche +3"), else 0
};
static std::vector<Item> s_Items;
static std::atomic<int> s_ItemsState{ 0 };  // 0 not started, 1 building, 2 ready, 3 failed
static std::atomic<int> s_ItemsProgress{ 0 };

struct NameTable {
    const wchar_t* param;
    int cat;
    const Cats* names;
    const char* kind;
    int group, icon;  // icon: where the row keeps its icon id
};
static const NameTable kNameTables[] = {
    { L"EquipParamGoods", GOODS, &GOODS_NAME, "Item", G_ITEM, Goods::ICON },
    { L"EquipParamWeapon", WEAPON, &WEAPON_NAME, "Weapon", G_WEAPON, WEAPON_ICON },
    { L"EquipParamProtector", PROTECTOR, &PROTECTOR_NAME, "Armor", G_ARMOR, PROTECTOR_ICON },
    { L"EquipParamAccessory", ACCESSORY, &ACCESSORY_NAME, "Talisman", G_TALISMAN, ACCESSORY_ICON },
    { L"EquipParamGem", GEM, &GEM_NAME, "Ash of War", G_ASH, GEM_ICON },
};

// Goods by their goodsType (EquipParamGoods +0x3e)
static const char* GoodsKind(int type, int& group) {
    switch (type) {
    case 1: group = G_KEY; return "Key item";
    case 2: group = G_ITEM; return "Crafting material";
    case 3: group = G_KEY; return "Remembrance";
    case 5: case 17: group = G_SPELL; return "Sorcery";
    case 16: case 18: group = G_SPELL; return "Incantation";
    case 7: case 8: group = G_SPIRIT; return "Spirit ash";
    case 9: group = G_KEY; return "Flask";
    case 10: group = G_ITEM; return "Crystal tear";
    case 12: group = G_KEY; return "Info";
    case 14: group = G_ITEM; return "Upgrade material";
    case 15: group = G_KEY; return "Great Rune";
    default: group = G_ITEM; return "Item";
    }
}
static int s_NameTable = 0, s_NameRow = 0;
static Table s_NameCur;
static std::unordered_map<std::string, int> s_ItemByName;  // name + kind -> s_Items index

static void BuildNames() {
    int budget = 1500;
    while (budget > 0 && s_NameTable < (int)(sizeof(kNameTables) / sizeof(kNameTables[0]))) {
        const NameTable& nt = kNameTables[s_NameTable];
        if (!s_NameCur.file) {
            s_NameCur = FindTable(nt.param);
            s_NameRow = 0;
            if (!s_NameCur.rows) {
                Log("DropFinder: no %ls table", nt.param);
                s_NameTable++;
                s_NameCur = {};
                continue;
            }
        }
        for (; s_NameRow < s_NameCur.rows && budget > 0; s_NameRow++, budget--) {
            int32_t id = s_NameCur.Id(s_NameRow);
            if (nt.cat == WEAPON && id % 10000) continue;
            std::string name = Text(*nt.names, id);
            // cut items the game never names ("[ERROR]Nomadic Merchant's Bell Bearing [11]")
            if (name.empty() || name.rfind("[ERROR]", 0) == 0 || name.find_first_not_of(" \r\n\t") == std::string::npos) continue;
            // spirit ashes and flasks come as one row per upgrade level ("... +3"): one entry for them all
            int level = 0;
            size_t plus = name.rfind(" +");
            if (nt.cat == GOODS && plus != std::string::npos && plus + 2 < name.size() &&
                name.find_first_not_of("0123456789", plus + 2) == std::string::npos) {
                level = atoi(name.c_str() + plus + 2);
                name.resize(plus);
            }
            uintptr_t row = s_NameCur.Data(s_NameRow);
            int group = nt.group;
            const char* kind = nt.cat == GOODS ? GoodsKind(Rd<uint8_t>(row + Goods::TYPE), group) : nt.kind;
            std::string key = name + '\x01' + kind;
            auto it = s_ItemByName.find(key);
            if (it != s_ItemByName.end()) {
                s_Items[it->second].keys.push_back(Key(nt.cat, id));
                s_Items[it->second].levels.push_back((int8_t)level);
                continue;
            }
            std::string lower = name;
            for (char& c : lower) c = (char)tolower((unsigned char)c);
            s_ItemByName.emplace(key, (int)s_Items.size());
            s_Items.push_back({ name, lower, kind, group, (int)Rd<uint16_t>(row + nt.icon), { Key(nt.cat, id) }, { (int8_t)level } });
        }
        if (s_NameRow >= s_NameCur.rows) {
            s_NameTable++;
            s_NameCur = {};
        }
        s_ItemsProgress = s_NameTable * 20;
    }
    if (s_NameTable >= (int)(sizeof(kNameTables) / sizeof(kNameTables[0]))) {
        std::sort(s_Items.begin(), s_Items.end(), [](const Item& a, const Item& b) { return a.lower < b.lower; });
        s_ItemByName.clear();
        Log("DropFinder: %d item names", (int)s_Items.size());
        s_ItemsState = s_Items.empty() ? 3 : 2;
    }
}

// ---- sources ----

struct Source {
    std::string what, note, where, route;
    bool done = false, placed = false;
    bool sure = true;  // done means the player got this item (not just whatever the lot rolled)
    uint32_t map = 0;  // the map it's placed in
    WPos pos;
    float dist = -1.0f;  // metres from the player (same space only)
    std::string label;   // for the tracker
};

static std::mutex s_Lock;
static std::atomic<int> s_WantItem{ -1 };  // s_Items index the finder window shows
static std::atomic<ULONGLONG> s_ShownAt{ 0 };  // when the finder window was last drawn
static int s_ResultItem = -1;              // under s_Lock
static std::vector<Source> s_Results;      // under s_Lock
static std::string s_ResultInfo, s_ResultCaption;  // under s_Lock: that item's summary line and description
static ULONGLONG s_NextResolve = 0;
static int s_LastResolved = -1;

static Table s_LotMap, s_LotEnemy, s_Npc, s_Shop, s_Custom, s_Goods;
static std::unordered_multimap<int32_t, int32_t> s_NpcByMapLot, s_NpcByEnemyLot;  // lot -> NpcParam id
static std::unordered_multimap<int64_t, uint32_t> s_LotFlags;  // Key -> the flags of the lot slots sure to give it
static bool s_TablesReady = false;

// The Key a lot or shop entry (category, id) counts under (-1 for none)
static int64_t EntryKey(int cat, int32_t id) {
    if (cat == CUSTOM_WEAPON) {
        uintptr_t r = s_Custom.rows ? s_Custom.Row(id) : 0;
        if (!r) return -1;
        cat = WEAPON;
        id = Rd<int32_t>(r);
    }
    if (cat == WEAPON) id = BaseWeapon(id);
    return Key(cat, id);
}

// A lot slot's flag: its own, else the lot's. The lot's flag is set whatever the roll gave, so it only says the
// player got this slot's item when the slot can't miss
static uint32_t SlotFlag(uintptr_t row, int slot, bool& sure) {
    uint32_t flag = Rd<uint32_t>(row + ItemLot::SLOT_FLAG + slot * 4);
    sure = flag != 0;
    if (flag) return flag;
    int total = 0;
    for (int k = 0; k < 8; k++) total += Rd<uint16_t>(row + ItemLot::BASE_POINT + k * 2);
    int pts = Rd<uint16_t>(row + ItemLot::BASE_POINT + slot * 2);
    flag = Rd<uint32_t>(row + ItemLot::FLAG);
    sure = flag && (total <= 0 || pts * 200 >= total * 199);
    return flag;
}

static bool LoadTables() {
    s_LotMap = FindTable(L"ItemLotParam_map");
    s_LotEnemy = FindTable(L"ItemLotParam_enemy");
    s_Npc = FindTable(L"NpcParam");
    s_Shop = FindTable(L"ShopLineupParam");
    s_Custom = FindTable(L"EquipParamCustomWeapon");
    s_Goods = FindTable(L"EquipParamGoods");
    if (!s_LotMap.rows || !s_LotEnemy.rows || !s_Npc.rows || !s_Shop.rows) return false;
    for (int i = 0; i < s_Npc.rows; i++) {
        uintptr_t r = s_Npc.Data(i);
        int32_t id = s_Npc.Id(i), lm = Rd<int32_t>(r + Npc::LOT_MAP), le = Rd<int32_t>(r + Npc::LOT_ENEMY);
        if (lm > 0) s_NpcByMapLot.emplace(lm, id);
        if (le > 0) s_NpcByEnemyLot.emplace(le, id);
    }
    for (int t = 0; t < 2; t++) {
        const Table& tab = t ? s_LotEnemy : s_LotMap;
        for (int i = 0; i < tab.rows; i++) {
            uintptr_t r = tab.Data(i);
            for (int s = 0; s < 8; s++) {
                int32_t id = Rd<int32_t>(r + ItemLot::ID + s * 4);
                int cat = Rd<int32_t>(r + ItemLot::CATEGORY + s * 4);
                bool sure;
                uint32_t flag = id > 0 && cat > 0 ? SlotFlag(r, s, sure) : 0;
                int64_t k = flag && sure ? EntryKey(cat, id) : -1;
                if (k >= 0) s_LotFlags.emplace(k, flag);
            }
        }
    }
    Log("DropFinder: tables: %d map lots, %d enemy lots, %d NPCs, %d shop rows, %d sure lot flags", s_LotMap.rows, s_LotEnemy.rows,
        s_Npc.rows, s_Shop.rows, (int)s_LotFlags.size());
    return true;
}

// Does a lot or shop entry (category, id) give one of these items
static bool Matches(const std::vector<int64_t>& keys, int cat, int32_t id) {
    int64_t k = EntryKey(cat, id);
    return k >= 0 && std::find(keys.begin(), keys.end(), k) != keys.end();
}

static const char* Compass(float dx, float dz) {
    static const char* kDirs[] = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" };
    float deg = atan2f(dx, dz) * 57.29578f;
    if (deg < 0) deg += 360.0f;
    return kDirs[(int)((deg + 22.5f) / 45.0f) % 8];
}

static float Flat(const WPos& a, const WPos& b) {
    float dx = b.x - a.x, dz = b.z - a.z;
    return sqrtf(dx * dx + dz * dz);
}

static std::string Metres(float m) {
    char b[32];
    if (m >= 1000.0f) snprintf(b, sizeof(b), "%.1f km", m / 1000.0f);
    else snprintf(b, sizeof(b), "%d m", (int)(m + 0.5f));
    return b;
}

static std::vector<int8_t> s_GraceOn;  // the graces' flags, read once per lookup

static bool Overworld(uint32_t map) { return (map >> 24) == 60 || (map >> 24) == 61; }

// "near <grace>", and how to get there: warp to it, or to the closest grace you have. Graces in the same
// dungeon (or out on the overworld for an overworld spot) come first: a cave's grace can sit right under a
// field pickup on the map
static void Describe(Source& s, const WPos& player, bool havePlayer) {
    const GraceInfo* nearest = nullptr;
    const GraceInfo* have = nullptr;
    float dn = 1e30f, dh = 1e30f;
    bool nearestHave = false;
    for (int pass = 0; pass < 2; pass++) {
        bool wantNearest = !nearest, wantHave = !have;  // pass 1 (any grace in this space) only fills in the gaps
        if (!wantNearest && !wantHave) break;
        const std::vector<GraceInfo>& graces = Graces();
        for (size_t i = 0; i < graces.size(); i++) {
            const GraceInfo& g = graces[i];
            if (g.pos.space != s.pos.space) continue;
            if (pass == 0 && (Overworld(s.map) ? !Overworld(g.map) : g.map != s.map)) continue;
            float d = Flat(g.pos, s.pos);
            int on = i < s_GraceOn.size() ? s_GraceOn[i] : -1;
            if (wantNearest && d < dn) { dn = d; nearest = &g; nearestHave = on == 1; }
            if (wantHave && on == 1 && d < dh) { dh = d; have = &g; }
        }
    }
    if (nearest) s.where = "near " + nearest->name + (nearestHave ? "" : " (grace not found yet)");
    if (have) {
        s.route = "Warp to " + have->name + ", then " + Metres(dh) + " " + Compass(s.pos.x - have->pos.x, s.pos.z - have->pos.z);
        float dy = s.pos.y - have->pos.y;
        if (fabsf(dy) > 15.0f) s.route += ", " + Metres(fabsf(dy)) + (dy > 0 ? " up" : " down");
    } else if (nearest) {
        s.route = "You have no grace in this area yet";
    }
    if (havePlayer && player.space == s.pos.space) s.dist = Flat(player, s.pos);
}

static void AddPlaced(std::vector<Source>& out, Source base, const Placed& at, const WPos& player, bool havePlayer) {
    base.placed = true;
    base.map = at.map & 0xffffff00u;
    ToWorld(at.map, at.x, at.y, at.z, base.pos);
    Describe(base, player, havePlayer);
    out.push_back(std::move(base));
}

static std::string NpcName(int32_t npc) {
    uintptr_t r = s_Npc.Row(npc);
    return r ? Text(NPC_NAME, Rd<int32_t>(r + Npc::NAME_ID)) : std::string();
}

// Characters placed with this NpcParam (or talk id), nearest first, at most `keep` (the rest counted)
static std::vector<int> Nearest(const std::unordered_multimap<int32_t, int>& by, int32_t key, const WPos& player, bool havePlayer,
                                size_t keep, int& more) {
    std::vector<std::pair<float, int>> all;
    auto range = by.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        const Chr& c = s_Idx.chars[it->second];
        WPos w;
        ToWorld(c.at.map, c.at.x, c.at.y, c.at.z, w);
        float d = havePlayer && w.space == player.space ? Flat(player, w) : 1e9f;
        all.push_back({ d, it->second });
    }
    std::sort(all.begin(), all.end());
    more = all.size() > keep ? (int)(all.size() - keep) : 0;
    std::vector<int> out;
    for (size_t i = 0; i < all.size() && i < keep; i++) out.push_back(all[i].second);
    return out;
}

struct LotHit {
    int32_t row;  // the row holding the item
    int slot;
    bool enemy;
};

// The lot and shop rows that give an item: found once per item (the tables don't change while playing)
static int s_HitsItem = -1;
static std::vector<LotHit> s_Hits;
static std::vector<int> s_ShopHits;  // ShopLineupParam row indices

static void FindRows(int item) {
    s_HitsItem = item;
    s_Hits.clear();
    s_ShopHits.clear();
    const std::vector<int64_t>& keys = s_Items[item].keys;
    for (int t = 0; t < 2; t++) {
        const Table& tab = t ? s_LotEnemy : s_LotMap;
        for (int i = 0; i < tab.rows; i++) {
            uintptr_t r = tab.Data(i);
            for (int s = 0; s < 8; s++) {
                int32_t id = Rd<int32_t>(r + ItemLot::ID + s * 4);
                int cat = Rd<int32_t>(r + ItemLot::CATEGORY + s * 4);
                if (id > 0 && cat > 0 && Matches(keys, cat, id)) s_Hits.push_back({ tab.Id(i), s, t == 1 });
            }
        }
    }
    static const int kShopCat[] = { WEAPON, PROTECTOR, ACCESSORY, GOODS, GEM, CUSTOM_WEAPON };
    for (int i = 0; i < s_Shop.rows; i++) {
        uintptr_t r = s_Shop.Data(i);
        int type = Rd<uint8_t>(r + Shop::EQUIP_TYPE);
        if (type <= 5 && Matches(keys, kShopCat[type], Rd<int32_t>(r + Shop::EQUIP_ID))) s_ShopHits.push_back(i);
    }
}

static void Resolve(int item, std::vector<Source>& out) {
    out.clear();
    if (s_HitsItem != item) FindRows(item);
    WPos player;
    float phys[3];
    bool havePlayer = Player(player, phys);
    const std::vector<GraceInfo>& graces = Graces();
    s_GraceOn.resize(graces.size());
    for (size_t i = 0; i < graces.size(); i++) s_GraceOn[i] = (int8_t)Flag(graces[i].flag);

    // item lots holding it; the game also rolls the rows right after a lot it awards, so a row counts for the
    // lots just before it too (as long as the ids run on)
    for (const LotHit& h : s_Hits) {
        const Table& tab = h.enemy ? s_LotEnemy : s_LotMap;
        uintptr_t r = tab.Row(h.row);
        int total = 0;
        for (int k = 0; k < 8; k++) total += Rd<uint16_t>(r + ItemLot::BASE_POINT + k * 2);
        int pts = Rd<uint16_t>(r + ItemLot::BASE_POINT + h.slot * 2);
        float chance = total > 0 ? 100.0f * pts / total : 100.0f;
        bool sure;
        uint32_t flag = SlotFlag(r, h.slot, sure);
        int num = Rd<uint8_t>(r + ItemLot::NUM + h.slot);
        bool done = flag && Flag(flag) == 1;
        char note[64] = "";
        if (num > 1) snprintf(note, sizeof(note), "x%d", num);
        if (chance < 99.5f) snprintf(note + strlen(note), sizeof(note) - strlen(note), "%s%.0f%% chance", *note ? ", " : "", chance);
        size_t before = out.size();
        for (int back = 0; back < 10; back++) {
            int32_t lot = h.row - back;
            if (back && tab.Find(lot) < 0) break;
            if (!h.enemy) {
                auto pr = s_Idx.pickups.equal_range(lot);
                for (auto it = pr.first; it != pr.second; ++it) {
                    Source s;
                    s.what = "Pickup";
                    s.note = note;
                    s.done = done, s.sure = sure;
                    AddPlaced(out, s, it->second, player, havePlayer);
                }
                auto ar = s_Idx.awards.equal_range(lot);
                for (auto it = ar.first; it != ar.second; ++it) {
                    Source s;
                    s.what = "Reward";
                    s.note = note;
                    s.done = done, s.sure = sure;
                    const Placed* at = nullptr;
                    Placed tmp{};
                    std::string who;
                    for (int32_t e : it->second.entities) {
                        auto c = s_Idx.byEntity.find(e);
                        if (c != s_Idx.byEntity.end()) {
                            tmp = s_Idx.chars[c->second].at;
                            at = &tmp;
                            who = NpcName(s_Idx.chars[c->second].npc);
                            break;
                        }
                        auto a = s_Idx.assets.find(e);
                        if (a != s_Idx.assets.end()) { at = &a->second; break; }
                    }
                    if (!who.empty()) s.what = "Reward: " + who;
                    if (!at) { tmp = { it->second.map, 0, 0, 0 }; at = &tmp; s.note += s.note.empty() ? "somewhere in this area" : ", somewhere in this area"; }
                    AddPlaced(out, s, *at, player, havePlayer);
                }
                auto gr = s_Idx.gifts.equal_range(lot);
                for (auto it = gr.first; it != gr.second; ++it) {
                    int more = 0;
                    for (int ci : Nearest(s_Idx.byTalk, it->second, player, havePlayer, 3, more)) {
                        Source s;
                        std::string who = NpcName(s_Idx.chars[ci].npc);
                        s.what = "Given by " + (who.empty() ? std::string("an NPC") : who);
                        s.note = note;
                        s.done = done, s.sure = sure;
                        AddPlaced(out, s, s_Idx.chars[ci].at, player, havePlayer);
                    }
                }
            }
            auto& byLot = h.enemy ? s_NpcByEnemyLot : s_NpcByMapLot;
            auto nr = byLot.equal_range(lot);
            for (auto it = nr.first; it != nr.second; ++it) {
                int more = 0;
                std::vector<int> spots = Nearest(s_Idx.byNpc, it->second, player, havePlayer, 3, more);
                std::string who = NpcName(it->second);
                for (size_t k = 0; k < spots.size(); k++) {
                    Source s;
                    s.what = "Dropped by " + (who.empty() ? std::string("an enemy") : who);
                    s.note = note;
                    if (k + 1 == spots.size() && more) s.note += (s.note.empty() ? "" : ", ") + std::to_string(more) + " more like it elsewhere";
                    s.done = done, s.sure = sure;
                    AddPlaced(out, s, s_Idx.chars[spots[k]].at, player, havePlayer);
                }
            }
        }
        // an overworld map lot nothing places (10XXZZ#### in m60, 20XXZZ#### in m61): somewhere in that square
        bool m60 = h.row >= 1000000000 && h.row < 1100000000, m61 = h.row >= 2000000000 && h.row < 2100000000;
        if (out.size() == before && !h.enemy && (m60 || m61)) {
            int gx = h.row / 1000000 % 100, gz = h.row / 10000 % 100;
            Source s;
            s.what = "Pickup";
            s.note = std::string(note) + (*note ? ", " : "") + "somewhere in this 256 m square";
            s.done = done, s.sure = sure;
            AddPlaced(out, s, { MapId(m60 ? 60 : 61, gx, gz), 0, 0, 0 }, player, havePlayer);
        }
    }

    // shops: the rows selling it, the talk scripts that open those rows, the people placed with those scripts
    for (int i : s_ShopHits) {
        uintptr_t r = s_Shop.Data(i);
        int32_t row = s_Shop.Id(i);
        int price = Rd<int32_t>(r + Shop::VALUE);
        uint32_t release = Rd<uint32_t>(r + Shop::RELEASE_FLAG);
        char note[96];
        if (price > 0) snprintf(note, sizeof(note), "%d %s", price, Rd<uint8_t>(r + Shop::COST_TYPE) == 0 ? "runes" : "(trade)");
        else snprintf(note, sizeof(note), "free");
        bool stocked = !release || Flag(release) == 1;
        std::string later = ", not in stock yet";
        auto unlock = stocked ? s_Idx.handInFlags.end() : s_Idx.handInFlags.find(release);
        if (unlock != s_Idx.handInFlags.end()) later = ", not in stock until you hand over " + Text(GOODS_NAME, unlock->second);
        std::vector<int32_t> talks;
        for (const ShopRange& sr : s_Idx.shops)
            if (row >= sr.lo && row <= sr.hi && std::find(talks.begin(), talks.end(), sr.talk) == talks.end()) talks.push_back(sr.talk);
        std::vector<std::string> seen;
        for (int32_t talk : talks) {
            int more = 0;
            for (int ci : Nearest(s_Idx.byTalk, talk, player, havePlayer, 2, more)) {
                std::string who = NpcName(s_Idx.chars[ci].npc);
                std::string id = who + std::to_string(s_Idx.chars[ci].at.map);
                if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
                seen.push_back(id);
                Source s;
                s.what = "Sold by " + (who.empty() ? std::string("a merchant") : who);
                s.note = std::string(note) + (stocked ? "" : later);
                AddPlaced(out, s, s_Idx.chars[ci].at, player, havePlayer);
            }
        }
    }

    for (Source& s : out) {
        s.label = s.what + (s.where.empty() ? "" : ", " + s.where);
    }
    std::stable_sort(out.begin(), out.end(), [](const Source& a, const Source& b) {
        if (a.done != b.done) return !a.done;
        float da = a.dist < 0 ? 1e9f : a.dist, db = b.dist < 0 ? 1e9f : b.dist;
        return da < db;
    });
    // the same thing in the same spot (a lot reached from two rows) once
    std::vector<Source> uniq;
    for (Source& s : out) {
        bool dup = false;
        for (const Source& u : uniq)
            if (u.what == s.what && u.note == s.note && u.pos.space == s.pos.space && fabsf(u.pos.x - s.pos.x) < 1 && fabsf(u.pos.z - s.pos.z) < 1) dup = true;
        if (!dup) uniq.push_back(std::move(s));
        if (uniq.size() >= 40) break;
    }
    out.swap(uniq);
}

// ---- what can be found at all ----
// The Keys some lot or shop row Resolve can place hands out (the same rules: a lot row counts for the rows just
// before it, overworld lot ids for their square, shop rows for the merchants placed with their talk scripts).
// Items nothing hands out are left out of the list, unless the player has them

static std::unordered_set<int64_t> s_Available;      // filled once on the game thread, then only read
static std::atomic<bool> s_AvailableReady{ false };

static bool LotPlaced(const Table& tab, int32_t row, bool enemy) {
    auto& byLot = enemy ? s_NpcByEnemyLot : s_NpcByMapLot;
    for (int back = 0; back < 10; back++) {
        int32_t lot = row - back;
        if (back && tab.Find(lot) < 0) break;
        if (!enemy && (s_Idx.pickups.count(lot) || s_Idx.awards.count(lot) || s_Idx.gifts.count(lot))) return true;
        auto nr = byLot.equal_range(lot);
        for (auto it = nr.first; it != nr.second; ++it)
            if (s_Idx.byNpc.count(it->second)) return true;
    }
    bool m60 = row >= 1000000000 && row < 1100000000, m61 = row >= 2000000000 && row < 2100000000;
    return !enemy && (m60 || m61);
}

static void FindAvailable() {
    std::unordered_set<int64_t> found;
    for (int t = 0; t < 2; t++) {
        const Table& tab = t ? s_LotEnemy : s_LotMap;
        for (int i = 0; i < tab.rows; i++) {
            uintptr_t r = tab.Data(i);
            int placed = -1;  // worked out on the first slot that needs it
            for (int k = 0; k < 8; k++) {
                int32_t id = Rd<int32_t>(r + ItemLot::ID + k * 4);
                int cat = Rd<int32_t>(r + ItemLot::CATEGORY + k * 4);
                int64_t key = id > 0 && cat > 0 ? EntryKey(cat, id) : -1;
                if (key < 0 || found.count(key)) continue;
                if (placed < 0) placed = LotPlaced(tab, tab.Id(i), t == 1);
                if (placed) found.insert(key);
            }
        }
    }
    static const int kShopCat[] = { WEAPON, PROTECTOR, ACCESSORY, GOODS, GEM, CUSTOM_WEAPON };
    for (int i = 0; i < s_Shop.rows; i++) {
        uintptr_t r = s_Shop.Data(i);
        int type = Rd<uint8_t>(r + Shop::EQUIP_TYPE);
        int64_t key = type <= 5 ? EntryKey(kShopCat[type], Rd<int32_t>(r + Shop::EQUIP_ID)) : -1;
        if (key < 0 || found.count(key)) continue;
        int32_t row = s_Shop.Id(i);
        for (const ShopRange& sr : s_Idx.shops)
            if (row >= sr.lo && row <= sr.hi && s_Idx.byTalk.count(sr.talk)) {
                found.insert(key);
                break;
            }
    }
    Log("DropFinder: %d items can be found somewhere", (int)found.size());
    s_Available.swap(found);
    s_AvailableReady = true;
}

// Game text markup: a line with a button icon ("Examine using <?keyicon@31?>") goes, <font ...> tags go
static std::string Clean(const std::string& text) {
    std::string out;
    for (size_t i = 0; i <= text.size();) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        i = end + 1;
        if (line.find("<?") != std::string::npos) continue;
        for (size_t a; (a = line.find("<font")) != std::string::npos || (a = line.find("</font")) != std::string::npos;) {
            size_t b = line.find('>', a);
            line.erase(a, b == std::string::npos ? std::string::npos : b - a + 1);
        }
        out += line;
        out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// The item's summary line and description, from the first of its ids that has them
static void ItemText(const Item& item, std::string& info, std::string& caption) {
    static const struct {
        int cat;
        const Cats *info, *caption;
    } kText[] = { { GOODS, &GOODS_INFO, &GOODS_CAPTION }, { WEAPON, &WEAPON_INFO, &WEAPON_CAPTION },
                  { PROTECTOR, &PROTECTOR_INFO, &PROTECTOR_CAPTION }, { ACCESSORY, &ACCESSORY_INFO, &ACCESSORY_CAPTION },
                  { GEM, &GEM_INFO, &GEM_CAPTION } };
    for (int64_t k : item.keys)
        for (const auto& t : kText) {
            if ((k >> 32) != t.cat) continue;
            int32_t id = (int32_t)(k & 0xffffffff);
            if (info.empty()) info = Clean(Text(*t.info, id));
            if (caption.empty()) caption = Clean(Text(*t.caption, id));
        }
}

// ---- what the player has ----
// EquipInventoryData (vtable 0x29e7f28): +0xc normal capacity, +0x10 normal entries, +0x1c key capacity, +0x20 key
// entries; an entry is 0x18 bytes {handle, item id with its category bits, quantity, ...}. The inventory is
// PlayerGameData ([GameDataMan+0x8]) +0x408, the storage chest [GameDataMan+0x58] +0x1180.

static constexpr uintptr_t GAME_DATA_MAN = 0x3d64fb8;
static constexpr uintptr_t INVENTORY_VTABLE = 0x29e7f28;

struct Owned {
    int carried = 0, stored = 0;
    int level = -1, affinity = 0;  // the best one held: its upgrade level, and a weapon's affinity (id / 100 % 100)
};
static std::unordered_map<int64_t, Owned> s_Owned;  // under s_Lock, by Key (weapons by base id)
static ULONGLONG s_NextOwned = 0;

static void ReadInventory(uintptr_t inv, bool chest, std::unordered_map<int64_t, Owned>& out) {
    if (!inv || Rd<uintptr_t>(inv) != Base() + INVENTORY_VTABLE) return;
    for (int list = 0; list < 2; list++) {
        uint32_t cap = Rd<uint32_t>(inv + (list ? 0x1c : 0x0c));
        uintptr_t entries = Ptr(inv + (list ? 0x20 : 0x10));
        if (!entries || cap > 0x2000) continue;
        for (uint32_t i = 0; i < cap; i++) {
            uintptr_t e = entries + i * 0x18;
            uint32_t handle = Rd<uint32_t>(e), item = Rd<uint32_t>(e + 4);
            int qty = Rd<int32_t>(e + 8);
            if (!handle || handle == 0xffffffffu || qty <= 0) continue;
            int32_t id = (int32_t)(item & 0x0fffffff);
            int64_t key;
            switch (item >> 28) {
            case 0: key = Key(WEAPON, BaseWeapon(id)); break;
            case 1: key = Key(PROTECTOR, id); break;
            case 2: key = Key(ACCESSORY, id); break;
            case 4: key = Key(GOODS, id); break;
            case 8: key = Key(GEM, id); break;
            default: continue;
            }
            Owned& o = out[key];
            (chest ? o.stored : o.carried) += qty;
            if ((item >> 28) == 0 && id % 100 > o.level) o.level = id % 100, o.affinity = id % 10000 / 100;
        }
    }
}

static void RefreshOwned() {
    std::unordered_map<int64_t, Owned> owned;
    uintptr_t gdm = Global(GAME_DATA_MAN);
    uintptr_t pgd = gdm ? Ptr(gdm + 0x8) : 0;
    uintptr_t storage = gdm ? Ptr(gdm + 0x58) : 0;
    if (pgd) ReadInventory(pgd + 0x408, false, owned);
    if (storage) ReadInventory(storage + 0x1180, true, owned);
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Owned.swap(owned);
}

// How many of an item the player has (all its ids), carried and in the chest; under s_Lock
static Owned OwnedOf(const Item& item) {
    Owned o;
    for (size_t j = 0; j < item.keys.size(); j++) {
        auto it = s_Owned.find(item.keys[j]);
        if (it == s_Owned.end()) continue;
        o.carried += it->second.carried;
        o.stored += it->second.stored;
        int level = (std::max)((int)item.levels[j], it->second.level);
        if (level > o.level) o.level = level, o.affinity = it->second.affinity;
    }
    return o;
}

// ---- weapon stats ----
// Attack power as the game works it out. Per damage type it is base x the upgrade's rate, plus base x the sum,
// over the attributes that type scales with (AttackElementCorrectParam), of scaling x the upgrade's rate x the
// curve (CalcCorrectGraph) at that attribute. A requirement not met for one of them takes 40% off instead.
// Status buildup comes from the weapon's SpEffects (moved along by the upgrade); Arcane adds to poison, bleed,
// sleep and madness.
// Damage a second: one round of the weapon's light-attack combo (DropFinder.txt's W lines, from the pack's animation
// events), each hit's motion values (BehaviorParam_PC -> AtkParam_Pc, percent of attack power by damage type) times
// the attack power, over the time the round takes. Every hit lands; defence, stamina and buildup aren't counted.

namespace Wep {  // EquipParamWeapon, 0x298; attributes in the order Str, Dex, Int, Fai, Arc; damage Phys, Mag, Fire, Ltng, Holy
constexpr int CORRECT[5] = { 0x24, 0x28, 0x2c, 0x30, 0x19c }, ATTACK[5] = { 0xc8, 0xca, 0xcc, 0xce, 0x18c },
              PROPER[5] = { 0xf2, 0xf3, 0xf4, 0xf5, 0x195 }, GRAPH[5] = { 0xec, 0x17d, 0x17e, 0x17f, 0x18e };
constexpr int BEHAVIOR = 0x44, REINFORCE_TYPE = 0xda, CRITICAL = 0xe0, ELEMENT_CORRECT = 0x22c;
constexpr int CASTS = 0x105, CASTS_SORCERY = 0x80, CASTS_INCANTATION = 0x200;  // u16 at +0x105: enableMagic, enableMiracle bits
constexpr int TYPE = 0x1a6;  // u16 wepType: the weapon class
constexpr int VARIATION = 0x04, MOVESET = 0xe7;  // s32 behaviorVariationId, u8 wepmotionCategory (the TAE of its attacks)
constexpr int GRAPH_POISON = 0x18f, GRAPH_BLOOD = 0x194, GRAPH_SLEEP = 0x23e, GRAPH_MADNESS = 0x23f;
}
namespace Reinforce {  // ReinforceParamWeapon, 0x80 (row = the weapon's reinforceTypeId + level)
constexpr int ATTACK[5] = { 0x00, 0x04, 0x08, 0x0c, 0x58 }, CORRECT[5] = { 0x1c, 0x20, 0x24, 0x28, 0x60 }, SP_EFFECT = 0x50;
constexpr int EQUIVALENT = 0x57;  // u8 maxReinforceLevel: the regular level this row counts as (a somber +10 is 25)
}
namespace ElemCorrect {  // AttackElementCorrectParam, 0x80: a bit per (damage type * 5 + attribute) from +0, then s16s
constexpr int OVERWRITE = 0x04, INFLUENCE = 0x36;
}
namespace Behavior {  // BehaviorParam_PC, 0x20: row 100000000 + variation * 1000 + judge id
constexpr int REF_TYPE = 0x09, REF_ID = 0x0c;  // u8 (0: an AtkParam_Pc row), s32
}
namespace AtkPc {  // AtkParam_Pc, 0x1c8: motion values (u16 percent) by damage type
constexpr int MOTION[5] = { 0x3e, 0x40, 0x42, 0x44, 0x186 };
}
// PlayerGameData: vigor, mind, endurance, strength, dexterity, intelligence, faith, arcane (u32), level at +0x68
constexpr int PLAYER_ATTRIBUTES = 0x3c, PLAYER_LEVEL = 0x68;

struct WeaponAsk {
    int item = -1, level = 0, affinity = 0;
    bool twoHand = false;
    bool operator==(const WeaponAsk& o) const { return item == o.item && level == o.level && affinity == o.affinity && twoHand == o.twoHand; }
};
struct Buildup {
    const char* name;
    float base, bonus;
};
struct WeaponView {
    WeaponAsk ask;
    bool on = false, statsKnown = false;
    int level = 0, affinity = 0, maxLevel = 0, critical = 100;
    std::vector<std::pair<int, std::string>> affinities;  // the infusions this weapon has rows for
    float base[5] = {}, bonus[5] = {};  // by damage type; bonus < 0: a requirement isn't met
    float scaling[5] = {};              // by attribute, at this level
    int need[5] = {}, have[5] = {};     // requirements, the player's attributes (Strength x1.5 two-handed)
    std::vector<Buildup> buildup;
    float sorcery = -1, incantation = -1;  // spell scaling of a staff or seal (-1: it doesn't cast them)
    float rate = -1, dualRate = -1;        // light-attack damage a second as held, and powerstanced with two (-1: none)
    float seconds = 0, dualSeconds = 0;    // a round of the combo
    int hits = 0, dualHits = 0;
};
static WeaponAsk s_WeaponAsk;    // under s_Lock: what the finder shows
static WeaponView s_WeaponView;  // under s_Lock
static WeaponAsk s_WeaponDone;   // game thread
static Table s_Weapon, s_Reinforce, s_Graph, s_ElemCorrect, s_SpEffect, s_Behavior, s_AtkPc;

static const char* const kAffinities[13] = { "Standard", "Heavy", "Keen", "Quality", "Fire", "Flame Art", "Lightning",
                                             "Sacred", "Magic", "Cold", "Poison", "Blood", "Occult" };

// CalcCorrectGraph at an attribute: stages (stageMaxVal0-4) to growth (stageMaxGrowVal0-4, percent), each stage
// curved by adjPt_maxGrowVal (x^e, or 1 - (1 - x)^-e for a negative one)
static float Curve(int graph, float x) {
    uintptr_t r = s_Graph.Row(graph);
    if (!r) return 0;
    float stage[5], grow[5], adj[5];
    for (int i = 0; i < 5; i++) stage[i] = Rd<float>(r + i * 4), grow[i] = Rd<float>(r + 0x14 + i * 4), adj[i] = Rd<float>(r + 0x28 + i * 4);
    x = std::clamp(x, stage[0], stage[4]);
    int i = 0;
    while (i < 3 && x > stage[i + 1]) i++;
    float span = stage[i + 1] - stage[i], ratio = span > 0 ? (x - stage[i]) / span : 0.0f;
    float g = adj[i] > 0 ? powf(ratio, adj[i]) : 1.0f - powf(1.0f - ratio, -adj[i]);
    return (grow[i] + (grow[i + 1] - grow[i]) * g) / 100.0f;
}

// Strength to Arcane, checked against the level they roughly add up to (Vins' classes don't start at the vanilla
// sum: one character is level 151 with attributes adding up to 154 + 79)
static bool PlayerAttributes(int out[5]) {
    uintptr_t gdm = Global(GAME_DATA_MAN);
    uintptr_t pgd = gdm ? Ptr(gdm + 0x8) : 0;
    if (!pgd) return false;
    int a[8], sum = 0;
    for (int i = 0; i < 8; i++) {
        a[i] = (int)Rd<uint32_t>(pgd + PLAYER_ATTRIBUTES + i * 4);
        if (a[i] < 1 || a[i] > 99) return false;
        sum += a[i];
    }
    int level = (int)Rd<uint32_t>(pgd + PLAYER_LEVEL);
    if (level < 1 || abs(sum - 79 - level) > 40) return false;
    for (int i = 0; i < 5; i++) out[i] = a[3 + i];
    return true;
}

static bool LoadWeaponTables() {
    if (!s_Weapon.rows) {
        s_Weapon = FindTable(L"EquipParamWeapon");
        s_Reinforce = FindTable(L"ReinforceParamWeapon");
        s_Graph = FindTable(L"CalcCorrectGraph");
        s_ElemCorrect = FindTable(L"AttackElementCorrectParam");
        s_SpEffect = FindTable(L"SpEffectParam");
        s_Behavior = FindTable(L"BehaviorParam_PC");
        s_AtkPc = FindTable(L"AtkParam_Pc");
    }
    return s_Weapon.rows && s_Reinforce.rows && s_Graph.rows && s_ElemCorrect.rows;
}

static int32_t BaseWeaponOf(const Item& item) {
    for (int64_t k : item.keys)
        if ((k >> 32) == WEAPON) return (int32_t)(k & 0xffffffff);
    return -1;
}

static int MaxLevel(int reinforceType) {
    int level = 0;
    while (level < 25 && s_Reinforce.Row(reinforceType + level + 1)) level++;
    return level;
}

// Damage per type and spell scaling of a weapon row at an upgrade (reinforce) row, with these attributes
struct Attack {
    float base[5] = {}, bonus[5] = {};  // bonus < 0: a requirement isn't met
    float sorcery = -1, incantation = -1;
    float Total() const {
        float t = 0;
        for (int i = 0; i < 5; i++)
            if (base[i] > 0) t += floorf(base[i]) + floorf(bonus[i]);
        return t;
    }
};
static Attack AttackOf(uintptr_t w, uintptr_t re, const int have[5], bool known) {
    Attack at;
    uintptr_t ec = s_ElemCorrect.Row(Rd<int32_t>(w + Wep::ELEMENT_CORRECT));
    // how much damage type t gains from the attributes (false: a requirement for one of them isn't met)
    auto scaled = [&](int t, float& mult) {
        int graph = Rd<uint8_t>(w + Wep::GRAPH[t]);
        bool met = true;
        mult = 0;
        for (int a = 0; a < 5; a++) {
            int bit = t * 5 + a;
            if (!((Rd<uint8_t>(ec + bit / 8) >> (bit % 8)) & 1)) continue;
            met = met && have[a] >= Rd<uint8_t>(w + Wep::PROPER[a]);
            int16_t over = Rd<int16_t>(ec + ElemCorrect::OVERWRITE + bit * 2);
            float correct = over >= 0 ? (float)over : Rd<float>(w + Wep::CORRECT[a]);
            float influence = Rd<int16_t>(ec + ElemCorrect::INFLUENCE + bit * 2) / 100.0f;
            mult += correct * Rd<float>(re + Reinforce::CORRECT[a]) / 100.0f * influence * Curve(graph, (float)have[a]);
        }
        return met;
    };
    for (int t = 0; t < 5; t++) {
        float b = Rd<uint16_t>(w + Wep::ATTACK[t]) * Rd<float>(re + Reinforce::ATTACK[t]);
        at.base[t] = b;
        float mult;
        if (b > 0 && ec && known) at.bonus[t] = scaled(t, mult) ? b * mult : -0.4f * b;
    }
    // a staff's sorcery scaling works like magic damage on a base of 100, a seal's incantation scaling like holy
    uint16_t casts = Rd<uint16_t>(w + Wep::CASTS);
    float mult;
    if (ec && known && (casts & Wep::CASTS_SORCERY)) at.sorcery = scaled(1, mult) ? 100.0f * (1.0f + mult) : 60.0f;
    if (ec && known && (casts & Wep::CASTS_INCANTATION)) at.incantation = scaled(4, mult) ? 100.0f * (1.0f + mult) : 60.0f;
    return at;
}

static bool MeleeType(int type);

// A hit's motion values by damage type (1 = 100%), for a weapon's behavior variation: the variation's own
// BehaviorParam_PC row, else its hundred's (unique weapons share their kind's), else the common one. False for a
// hit that isn't a swing (a bullet) or has no row. Game thread; kept, as params don't change.
static bool MotionValues(int32_t variation, int32_t judge, float mv[5]) {
    static std::unordered_map<int64_t, std::pair<bool, std::array<float, 5>>> known;
    int64_t key = (int64_t)variation << 32 | (uint32_t)judge;
    auto it = known.find(key);
    if (it == known.end()) {
        std::pair<bool, std::array<float, 5>> m{ false, {} };
        for (int32_t v : { variation, variation / 100 * 100, 0 }) {
            uintptr_t b = s_Behavior.rows ? s_Behavior.Row(100000000 + v * 1000 + judge) : 0;
            if (!b) continue;
            uintptr_t a = Rd<uint8_t>(b + Behavior::REF_TYPE) == 0 && s_AtkPc.rows ? s_AtkPc.Row(Rd<int32_t>(b + Behavior::REF_ID)) : 0;
            if (a) {
                m.first = true;
                for (int t = 0; t < 5; t++) m.second[t] = Rd<uint16_t>(a + AtkPc::MOTION[t]) / 100.0f;
            }
            break;
        }
        it = known.emplace(key, m).first;
    }
    if (it->second.first) memcpy(mv, it->second.second.data(), sizeof(float) * 5);
    return it->second.first;
}

static const Combo* ComboOf(uintptr_t w, int grip) {
    if (s_IdxState.load() != 2 || !MeleeType(Rd<uint16_t>(w + Wep::TYPE))) return nullptr;
    auto it = s_Idx.combos.find(Rd<uint8_t>(w + Wep::MOVESET) * 4 + grip);
    return it == s_Idx.combos.end() ? nullptr : &it->second;
}

// Damage over a round of a combo from one hand's hits (0 right, 1 left) or all of them (2), with a weapon row and
// its attack power; hits: how many of them have motion values
static float ComboDamage(const Combo& c, uintptr_t w, const Attack& at, int hands, int* hits = nullptr) {
    int32_t variation = Rd<int32_t>(w + Wep::VARIATION);
    float total = 0;
    if (hits) *hits = 0;
    for (const ComboHit& h : c.hits) {
        if (hands != 2 && h.left != (hands == 1)) continue;
        float best = -1;
        for (int k = 0; k < h.judges; k++) {
            float mv[5], d = 0;
            if (!MotionValues(variation, h.judge[k], mv)) continue;
            for (int t = 0; t < 5; t++)
                if (at.base[t] > 0) d += (floorf(at.base[t]) + floorf(at.bonus[t])) * mv[t];
            best = (std::max)(best, d);
        }
        if (best < 0) continue;
        total += best;
        if (hits) ++*hits;
    }
    return total;
}

static WeaponView ComputeWeapon(const WeaponAsk& ask) {
    WeaponView v;
    v.ask = ask;
    if (!LoadWeaponTables()) return v;
    int32_t base = BaseWeaponOf(s_Items[ask.item]);
    if (base < 0) return v;
    for (int a = 0; a < 20; a++)
        if (s_Weapon.Row(base + a * 100)) v.affinities.push_back({ a, a < 13 ? kAffinities[a] : "Affinity " + std::to_string(a) });
    v.affinity = s_Weapon.Row(base + ask.affinity * 100) ? ask.affinity : 0;
    uintptr_t w = s_Weapon.Row(base + v.affinity * 100);
    if (!w) return v;
    int type = Rd<int16_t>(w + Wep::REINFORCE_TYPE);
    v.maxLevel = MaxLevel(type);
    v.level = std::clamp(ask.level, 0, v.maxLevel);
    uintptr_t re = s_Reinforce.Row(type + v.level);
    if (!re) return v;
    v.on = true;
    v.statsKnown = PlayerAttributes(v.have);
    int oneHanded[5];
    memcpy(oneHanded, v.have, sizeof(oneHanded));
    if (v.statsKnown && ask.twoHand) v.have[0] = (std::min)(v.have[0] * 3 / 2, 148);
    for (int a = 0; a < 5; a++) {
        v.need[a] = Rd<uint8_t>(w + Wep::PROPER[a]);
        v.scaling[a] = Rd<float>(w + Wep::CORRECT[a]) * Rd<float>(re + Reinforce::CORRECT[a]);
    }
    v.critical = 100 + Rd<int16_t>(w + Wep::CRITICAL);
    Attack at = AttackOf(w, re, v.have, v.statsKnown);
    memcpy(v.base, at.base, sizeof(v.base));
    memcpy(v.bonus, at.bonus, sizeof(v.bonus));
    v.sorcery = at.sorcery;
    v.incantation = at.incantation;
    if (const Combo* c = ComboOf(w, ask.twoHand ? TWO_HANDED : ONE_HANDED)) {
        v.rate = ComboDamage(*c, w, at, 2, &v.hits) / c->seconds;
        v.seconds = c->seconds;
    }
    if (const Combo* c = ComboOf(w, POWERSTANCE)) {  // one-handed attributes: a weapon in each hand
        Attack one = ask.twoHand ? AttackOf(w, re, oneHanded, v.statsKnown) : at;
        v.dualRate = ComboDamage(*c, w, one, 2, &v.dualHits) / c->seconds;
        v.dualSeconds = c->seconds;
    }
    static const struct {
        const char* name;
        int at, graph;  // SpEffectParam buildup (s32); the weapon's curve for Arcane's share, -1 for none
    } kBuildup[] = { { "Poison", 0xcc, Wep::GRAPH_POISON }, { "Scarlet Rot", 0xd0, -1 }, { "Bleed", 0xd4, Wep::GRAPH_BLOOD },
                     { "Death Blight", 0xd8, -1 }, { "Frost", 0x1a8, -1 }, { "Sleep", 0x338, Wep::GRAPH_SLEEP },
                     { "Madness", 0x33c, Wep::GRAPH_MADNESS } };
    float arcane = v.scaling[4] / 100.0f;
    for (int k = 0; k < 3 && s_SpEffect.rows; k++) {
        int32_t id = Rd<int32_t>(w + Wep::BEHAVIOR + k * 4);
        if (id <= 0) continue;
        uintptr_t sp = s_SpEffect.Row(id + Rd<uint8_t>(re + Reinforce::SP_EFFECT + k));
        if (!sp) sp = s_SpEffect.Row(id);
        for (const auto& b : kBuildup) {
            int value = sp ? Rd<int32_t>(sp + b.at) : 0;
            if (value <= 0) continue;
            float bonus = b.graph >= 0 && arcane > 0 && v.statsKnown ? value * arcane * Curve(Rd<uint8_t>(w + b.graph), (float)v.have[4]) : 0.0f;
            v.buildup.push_back({ b.name, (float)value, bonus });
        }
    }
    return v;
}

// ---- best weapons for you ----
// Every weapon you could get (or have), at its best affinity for your attributes, upgraded to a level you pick (a
// somber weapon to the level that counts the same), ranked by attack power or by light-attack damage a second.
// Dual wielding (powerstance) needs two weapons of the same class: the best pair of each class, by attack power
// added up, or by the right hand's hits of one and the left hand's of the other in the powerstance combo.

struct WeaponClass {
    int type;  // wepType
    const char* name;
    bool melee;
};
static const WeaponClass kClasses[] = {
    { 1, "Dagger", true }, { 3, "Straight Sword", true }, { 5, "Greatsword", true }, { 7, "Colossal Sword", true },
    { 9, "Curved Sword", true }, { 11, "Curved Greatsword", true }, { 13, "Katana", true }, { 14, "Twinblade", true },
    { 15, "Thrusting Sword", true }, { 16, "Heavy Thrusting Sword", true }, { 17, "Axe", true }, { 19, "Greataxe", true },
    { 21, "Hammer", true }, { 23, "Great Hammer", true }, { 24, "Flail", true }, { 25, "Spear", true },
    { 28, "Great Spear", true }, { 29, "Halberd", true }, { 31, "Reaper", true }, { 35, "Fist", true }, { 37, "Claw", true },
    { 39, "Whip", true }, { 41, "Colossal Weapon", true }, { 87, "Torch", true }, { 88, "Hand-to-Hand Art", true },
    { 89, "Perfume Bottle", true }, { 91, "Throwing Blade", true }, { 92, "Backhand Blade", true },
    { 93, "Light Greatsword", true }, { 94, "Great Katana", true }, { 95, "Beast Claw", true },
    { 50, "Light Bow", false }, { 51, "Bow", false }, { 53, "Greatbow", false }, { 55, "Crossbow", false }, { 56, "Ballista", false },
    { 57, "Glintstone Staff", false }, { 61, "Sacred Seal", false }, { 65, "Small Shield", false }, { 67, "Medium Shield", false },
    { 69, "Greatshield", false }, { 90, "Thrusting Shield", false },
};
constexpr int kClassCount = (int)(sizeof(kClasses) / sizeof(kClasses[0]));
// The ranking's choices: these first, then one class each (kGroupCount + its index in kClasses)
enum { F_MELEE, F_RANGED, F_SORCERY, F_INCANTATION, F_SHIELDS, kGroupCount };
static const char* const kFilterNames[kGroupCount] = { "Melee weapons", "Bows and crossbows", "Sorcery (staves and seals)",
                                                       "Incantations (staves and seals)", "Shields" };

static int ClassOf(int type) {
    for (int i = 0; i < kClassCount; i++)
        if (kClasses[i].type == type) return i;
    return -1;
}
static bool MeleeType(int type) {
    int cls = ClassOf(type);
    return cls >= 0 && kClasses[cls].melee;
}
static bool MeleeFilter(int filter) {
    return filter == F_MELEE || (filter >= kGroupCount && kClasses[filter - kGroupCount].melee);
}
static bool InFilter(int filter, int cls, uint16_t casts) {
    if (filter == F_SORCERY) return (casts & Wep::CASTS_SORCERY) != 0;
    if (filter == F_INCANTATION) return (casts & Wep::CASTS_INCANTATION) != 0;
    if (cls < 0) return false;
    int type = kClasses[cls].type;
    if (filter == F_MELEE) return kClasses[cls].melee;
    if (filter == F_RANGED) return type >= 50 && type <= 56;
    if (filter == F_SHIELDS) return type == 65 || type == 67 || type == 69 || type == 90;
    return filter - kGroupCount == cls;
}

struct SuggestAsk {
    int filter = F_MELEE, level = -1;  // the regular upgrade level to compare at (-1: the best you have now)
    bool twoHand = false, dual = false, perSecond = false, onlyAvailable = true;
    bool operator==(const SuggestAsk& o) const {
        return filter == o.filter && level == o.level && twoHand == o.twoHand && dual == o.dual && perSecond == o.perSecond &&
               onlyAvailable == o.onlyAvailable;
    }
};
struct Suggestion {
    int item = -1, affinity = 0, level = 0, cls = -1;
    int item2 = -1, affinity2 = 0, level2 = 0;  // the left hand of a dual-wield pair
    float value = 0;    // attack power (both of a pair), or spell scaling
    float score = 0;    // what it's ranked by
    float rate = -1;    // light-attack damage a second (-1: not worked out)
    float right = 0, left = 0;  // powerstanced: that from the right hand's hits, the left's
    float seconds = 0;  // a round of the combo
    int hits = 0, leftHits = 0;  // in a round (powerstanced: the right hand's, the left's)
    bool owned = false;
    std::string label;  // "Keen Uchigatana +25", or both of a pair
};
struct SuggestView {
    SuggestAsk ask;
    bool on = false, statsKnown = false;
    bool perSecond = false;        // ranked by damage a second (asked for, and there are combos to go by)
    int level = 0, yourLevel = 0;  // compared at; the best level you have
    std::vector<Suggestion> list;
    Suggestion yours;              // your best now (item -1: none)
};
static SuggestAsk s_SuggestAsk;    // under s_Lock
static SuggestView s_SuggestView;  // under s_Lock
static std::atomic<ULONGLONG> s_SuggestShownAt{ 0 };
static SuggestAsk s_SuggestDone;   // game thread
static ULONGLONG s_NextSuggest = 0;

static std::string WeaponLabel(int32_t base, int affinity, int level) {
    std::string name = Text(WEAPON_NAME, base + affinity * 100);
    if (name.empty()) name = Text(WEAPON_NAME, base);
    return level > 0 ? name + " +" + std::to_string(level) : name;
}

static SuggestView ComputeSuggestions(const SuggestAsk& ask) {
    SuggestView v;
    v.ask = ask;
    int have[5];
    if (!LoadWeaponTables() || !(v.statsKnown = PlayerAttributes(have))) return v;
    v.on = true;
    bool melee = MeleeFilter(ask.filter), dual = ask.dual && melee;
    v.perSecond = ask.perSecond && melee && s_IdxState.load() == 2 && !s_Idx.combos.empty();
    if (ask.twoHand && !dual) have[0] = (std::min)(have[0] * 3 / 2, 148);
    std::unordered_map<int64_t, Owned> owned;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        owned = s_Owned;
    }
    // the upgrade level you've reached: the best weapon you have, as a regular level
    for (const auto& [k, o] : owned) {
        if ((k >> 32) != WEAPON || o.level < 0) continue;
        uintptr_t w = s_Weapon.Row((int32_t)(k & 0xffffffff) + o.affinity * 100);
        uintptr_t re = w ? s_Reinforce.Row(Rd<int16_t>(w + Wep::REINFORCE_TYPE) + o.level) : 0;
        if (re) v.yourLevel = (std::max)(v.yourLevel, (int)Rd<uint8_t>(re + Reinforce::EQUIVALENT));
    }
    v.level = ask.level >= 0 ? (std::min)(ask.level, 25) : v.yourLevel;

    // a weapon row at an upgrade: what it's worth (0: nothing to rank it by)
    auto rate = [&](uintptr_t w, uintptr_t re, Suggestion& s) {
        Attack at = AttackOf(w, re, have, true);
        if (ask.filter == F_SORCERY) return s.score = s.value = at.sorcery;
        if (ask.filter == F_INCANTATION) return s.score = s.value = at.incantation;
        s.value = at.Total();
        if (!v.perSecond) return s.score = s.value;
        const Combo* c = ComboOf(w, dual ? POWERSTANCE : ask.twoHand ? TWO_HANDED : ONE_HANDED);
        if (!c) return s.score = 0;
        s.seconds = c->seconds;
        if (dual) {  // two of it; a pair is put together from the hands' shares below
            int r, l;
            s.right = ComboDamage(*c, w, at, 0, &r) / c->seconds;
            s.left = ComboDamage(*c, w, at, 1, &l) / c->seconds;
            s.rate = s.right + s.left;
            s.hits = r, s.leftHits = l;
        } else {
            s.rate = ComboDamage(*c, w, at, 2, &s.hits) / c->seconds;
        }
        return s.score = s.rate;
    };
    std::vector<Suggestion> all, mine;
    for (int i = 0; i < (int)s_Items.size(); i++) {
        if (s_Items[i].group != G_WEAPON) continue;
        int32_t base = BaseWeaponOf(s_Items[i]);
        uintptr_t w0 = base >= 0 ? s_Weapon.Row(base) : 0;
        if (!w0) continue;
        int cls = ClassOf(Rd<uint16_t>(w0 + Wep::TYPE));
        if (!InFilter(ask.filter, cls, Rd<uint16_t>(w0 + Wep::CASTS))) continue;
        auto ow = owned.find(Key(WEAPON, base));
        bool isOwned = ow != owned.end() && ow->second.carried + ow->second.stored > 0;
        // as you have it now
        if (isOwned && ow->second.level >= 0) {
            uintptr_t w = s_Weapon.Row(base + ow->second.affinity * 100);
            uintptr_t re = w ? s_Reinforce.Row(Rd<int16_t>(w + Wep::REINFORCE_TYPE) + ow->second.level) : 0;
            Suggestion m{ i, ow->second.affinity, ow->second.level, cls };
            if (re) rate(w, re, m);
            m.owned = true;
            if (m.score > 0) mine.push_back(m);
        }
        if (ask.onlyAvailable && !isOwned && !s_Available.count(Key(WEAPON, base))) continue;
        // at its best affinity, upgraded to the level asked for
        Suggestion best{ i, 0, 0, cls };
        for (int a = 0; a < 20; a++) {
            uintptr_t w = s_Weapon.Row(base + a * 100);
            if (!w) continue;
            int type = Rd<int16_t>(w + Wep::REINFORCE_TYPE), level = 0, top = MaxLevel(type);
            for (int l = 1; l <= top; l++) {
                uintptr_t r = s_Reinforce.Row(type + l);
                if (r && Rd<uint8_t>(r + Reinforce::EQUIVALENT) <= v.level) level = l;
            }
            uintptr_t re = s_Reinforce.Row(type + level);
            if (!re) continue;
            Suggestion s{ i, a, level, cls };
            if (rate(w, re, s) > best.score) best = s;
        }
        best.owned = isOwned;
        if (best.score > 0) all.push_back(best);
    }
    auto byScore = [](const Suggestion& a, const Suggestion& b) { return a.score > b.score; };
    std::sort(all.begin(), all.end(), byScore);
    std::sort(mine.begin(), mine.end(), byScore);
    auto label = [&](Suggestion& s) {
        s.label = WeaponLabel(BaseWeaponOf(s_Items[s.item]), s.affinity, s.level);
    };
    if (dual) {
        // the best pair of each class: the two with the most attack power (all and mine are sorted, so the first
        // two found), or the right hand of one and the left of another with the most damage a second
        auto pairs = [&](std::vector<Suggestion>& from) {
            std::vector<Suggestion> out;
            for (int c = 0; c < kClassCount; c++) {
                const Suggestion *r = nullptr, *l = nullptr;
                for (const Suggestion& a : from) {
                    if (a.cls != c) continue;
                    for (const Suggestion& b : from) {
                        if (b.cls != c || &b == &a) continue;
                        if (!r || (v.perSecond ? a.right + b.left > r->right + l->left : false)) r = &a, l = &b;
                        if (!v.perSecond) break;
                    }
                    if (!v.perSecond && r) break;
                }
                if (!r) continue;
                Suggestion p = *r, second = *l;
                label(p);
                label(second);
                p.label += "  +  " + second.label;
                p.item2 = l->item, p.affinity2 = l->affinity, p.level2 = l->level;
                p.value += l->value;
                if (v.perSecond) {
                    p.rate = r->right + l->left;
                    p.score = p.rate;
                    p.leftHits = l->leftHits;
                } else {
                    p.score += l->score;
                }
                p.owned = r->owned && l->owned;
                out.push_back(p);
            }
            std::sort(out.begin(), out.end(), byScore);
            return out;
        };
        v.list = pairs(all);
        std::vector<Suggestion> minePairs = pairs(mine);
        if (!minePairs.empty()) v.yours = minePairs[0];
    } else {
        if (all.size() > 150) all.resize(150);
        for (Suggestion& s : all) label(s);
        v.list.swap(all);
        if (!mine.empty()) {
            v.yours = mine[0];
            label(v.yours);
        }
    }
    return v;
}

// ---- armor stats ----

namespace Armor {  // EquipParamProtector, 0x1a0
constexpr int CUT[8] = { 0xe4, 0xec, 0xe8, 0xf0, 0xf4, 0xf8, 0xfc, 0x11c };  // f32: physical, strike, slash, pierce, magic, fire, lightning, holy
constexpr int RESIST[4] = { 0xc4, 0xc8, 0x0c, 0xca };  // u16: poison (immunity), bleed (robustness), sleep (focus), death (vitality)
constexpr int POISE = 0x14, WEIGHT = 0x24;              // f32 (poise as a fraction: x1000 on screen)
}
struct ArmorView {
    int item = -1;
    bool on = false;
    float negation[8] = {}, poise = 0, weight = 0;
    int resist[4] = {};
};
static ArmorView s_ArmorView;  // under s_Lock
static Table s_Protector;

static ArmorView ComputeArmor(int item) {
    ArmorView v;
    v.item = item;
    if (!s_Protector.rows) s_Protector = FindTable(L"EquipParamProtector");
    uintptr_t r = 0;
    for (int64_t k : s_Items[item].keys)
        if (!r && (k >> 32) == PROTECTOR) r = s_Protector.Row((int32_t)(k & 0xffffffff));
    if (!r) return v;
    v.on = true;
    for (int i = 0; i < 8; i++) v.negation[i] = (1.0f - Rd<float>(r + Armor::CUT[i])) * 100.0f;
    for (int i = 0; i < 4; i++) v.resist[i] = Rd<uint16_t>(r + Armor::RESIST[i]);
    v.poise = Rd<float>(r + Armor::POISE) * 1000.0f;
    v.weight = Rd<float>(r + Armor::WEIGHT);
    return v;
}

// ---- what became of it ----
// An item the player may not hold any more: picked up from a lot slot that can't miss, or handed over to someone (a
// talk script takes it and sets a flag; only for one-of-a-kind goods, a stack can be handed over and found again)

struct Got {
    bool picked = false;
    std::string handedTo;  // who took it, "" for no one
};
static std::unordered_map<int, Got> s_Got;  // under s_Lock, by s_Items index: the items the window shows
static std::vector<int> s_Asked;            // under s_Lock: those items
static uint32_t s_AskedVersion = 0, s_GotVersion = ~0u;  // under s_Lock
static std::atomic<bool> s_Guest{ false };  // a Seamless guest: the flags read are the host's world's

static bool OneOfAKind(int32_t goods) {
    uintptr_t r = s_Goods.rows ? s_Goods.Row(goods) : 0;
    return r && Rd<int16_t>(r + Goods::MAX_NUM) == 1;
}

static Got GotOf(const Item& item) {
    Got g;
    for (int64_t k : item.keys) {
        auto lots = s_LotFlags.equal_range(k);
        int checked = 0;
        for (auto it = lots.first; it != lots.second && !g.picked && checked < 400; ++it, checked++) g.picked = Flag(it->second) == 1;
        int32_t goods = (int32_t)(k & 0xffffffff);
        if ((k >> 32) != GOODS || !g.handedTo.empty() || !OneOfAKind(goods)) continue;
        auto hand = s_Idx.handIns.equal_range(goods);
        for (auto it = hand.first; it != hand.second; ++it) {
            if (Flag(it->second.flag) != 1) continue;
            auto c = s_Idx.byTalk.find(it->second.talk);
            g.handedTo = c != s_Idx.byTalk.end() ? NpcName(s_Idx.chars[c->second].npc) : std::string();
            if (g.handedTo.empty()) g.handedTo = "someone";
            break;
        }
    }
    return g;
}

static void RefreshGot() {
    std::vector<int> asked;
    uint32_t version;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        asked = s_Asked;
        version = s_AskedVersion;
    }
    std::unordered_map<int, Got> got;
    for (int i : asked)
        if (i >= 0 && i < (int)s_Items.size()) got[i] = GotOf(s_Items[i]);
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Got.swap(got);
    s_GotVersion = version;
}

// ---- tracking ----

struct Track {
    bool on = false;
    WPos pos;
    std::string label;
};
static Track s_Track;          // under s_Lock (the menu sets it, the game thread reads it)
struct TrackView {
    bool on = false, same = false, haveHeading = false;
    float dist = 0, dy = 0, rel = 0;
    std::string label;
};
static TrackView s_TrackView;  // under s_Lock

static void UpdateTracker() {
    Track t;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        t = s_Track;
    }
    TrackView v;
    if (t.on) {
        v.on = true;
        v.label = t.label;
        WPos p;
        float phys[3];
        if (Player(p, phys) && p.space == t.pos.space) {
            v.same = true;
            float dx = t.pos.x - p.x, dz = t.pos.z - p.z;
            v.dist = sqrtf(dx * dx + dz * dz);
            v.dy = t.pos.y - p.y;
            float heading;
            if (CameraHeading(phys, heading)) {
                v.haveHeading = true;
                v.rel = atan2f(dx, dz) * 57.29578f - heading;
            }
        }
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    s_TrackView = v;
}

// ---- the game thread ----

void RunFrame() {
    CheckCode();
    if (!Base()) return;
    ULONGLONG now = GetTickCount64();
    bool shown = now - s_ShownAt.load() < 1000;  // only while the finder window is up
    int want = shown ? s_WantItem.load() : -1;
    bool needed = shown;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        needed = needed || s_Track.on;
    }
    if (!needed) return;
    if (!World::Ready()) return;
    if (s_ItemsState.load() == 1) BuildNames();
    UpdateTracker();
    bool tick = shown && now >= s_NextOwned;
    if (tick) {
        s_NextOwned = now + 1000;
        RefreshOwned();
        s_Guest = CoopResync::InHostsWorld();
    }
    if (!shown || s_ItemsState.load() != 2 || s_IdxState.load() != 2) return;
    if (!s_TablesReady) {
        s_TablesReady = LoadTables();
        if (!s_TablesReady) return;
        FindAvailable();
    }
    bool asked;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        asked = s_GotVersion != s_AskedVersion;
    }
    if (tick || asked) RefreshGot();
    if (now - s_SuggestShownAt.load() < 1000) {
        SuggestAsk ask;
        {
            std::lock_guard<std::mutex> lock(s_Lock);
            ask = s_SuggestAsk;
        }
        if (!(ask == s_SuggestDone) || now >= s_NextSuggest) {  // again every few seconds: attributes and inventory change
            SuggestView v = ComputeSuggestions(ask);
            s_SuggestDone = ask;
            s_NextSuggest = now + 3000;
            std::lock_guard<std::mutex> lock(s_Lock);
            s_SuggestView = std::move(v);
        }
    }
    if (want < 0) return;
    WeaponAsk ask;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        ask = s_WeaponAsk;
    }
    if (ask.item == want && s_Items[want].group == G_WEAPON && (tick || !(ask == s_WeaponDone))) {
        WeaponView v = ComputeWeapon(ask);
        s_WeaponDone = ask;
        std::lock_guard<std::mutex> lock(s_Lock);
        s_WeaponView = std::move(v);
    }
    if (want == s_LastResolved && now < s_NextResolve) return;
    std::vector<Source> res;
    Resolve(want, res);
    bool fresh = want != s_LastResolved;
    std::string info, caption;
    ArmorView armor;
    if (fresh) {
        Log("DropFinder: %s: %d sources", s_Items[want].name.c_str(), (int)res.size());
        ItemText(s_Items[want], info, caption);
        if (s_Items[want].group == G_ARMOR) armor = ComputeArmor(want);
    }
    s_LastResolved = want;
    s_NextResolve = now + 2000;  // distances and flags change as you play
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Results.swap(res);
    s_ResultItem = want;
    if (fresh) {
        s_ResultInfo.swap(info);
        s_ResultCaption.swap(caption);
        s_ArmorView = armor;
    }
}

// ---- menu ----

static char s_Query[96] = "";
static int s_Filter = -1;  // a Group, or -1 for everything
static std::vector<int> s_Matches;
static std::string s_MatchedQuery = "\x01";
static int s_MatchedFilter = -2;
static bool s_MatchedAvailable = false;
static int s_Selected = -1;
static bool s_WindowOpen = false;
static bool s_ShowCaption = false, s_FocusSearch = false, s_ScrollToSelected = false;
static int s_LastWindowFrame = -10, s_PlaceFrames = 0;

constexpr float kWindowW = 900.0f, kWindowH = 620.0f;  // at 1080p

namespace Col {  // the F8 menu's colours
constexpr ImU32 TEXT = IM_COL32(226, 214, 176, 255), BRIGHT = IM_COL32(242, 228, 190, 255), GOLD = IM_COL32(181, 173, 133, 255),
                DIM = IM_COL32(158, 148, 122, 255), FAINT = IM_COL32(118, 110, 92, 255), GREEN = IM_COL32(140, 191, 115, 255),
                CARD = IM_COL32(31, 28, 22, 255), CARD_DONE = IM_COL32(22, 20, 16, 255), EDGE = IM_COL32(78, 71, 55, 255),
                ICON_BG = IM_COL32(10, 9, 7, 255);
}

void DrawMenuSection(float scale) {
    const ImVec4 gold(0.71f, 0.68f, 0.52f, 1.0f), dim(0.62f, 0.58f, 0.48f, 1.0f);
    bool tracking;
    std::string trackLabel;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        tracking = s_Track.on;
        trackLabel = s_Track.label;
    }
    ImGui::Spacing();
    ImGui::TextColored(gold, "Find an item");
    ImGui::Separator();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(s_WindowOpen ? "Close the item finder" : "Find an item...", ImVec2(w, 0))) s_WindowOpen = !s_WindowOpen;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search any item, weapon, spell or talisman: where the game hands it out,\n"
                          "the nearest grace and the closest one you can warp to");
    if (tracking) {
        ImGui::TextColored(dim, "Tracking %s", trackLabel.c_str());
        if (ImGui::Button("Stop tracking", ImVec2(w, 0))) {
            std::lock_guard<std::mutex> lock(s_Lock);
            s_Track.on = false;
        }
    }
}

bool WindowOpen() { return s_WindowOpen; }

float WindowWidth(float scale) { return (std::min)(kWindowW * scale, ImGui::GetIO().DisplaySize.x - 32.0f * scale); }

// One line of text at this size, cut short with "..." where it would pass maxX
// 2961.4 -> "2,961"
static std::string Grouped(float value) {
    std::string digits = std::to_string((long long)(value + 0.5f)), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i && (digits.size() - i) % 3 == 0 && digits[i - 1] != '-') out += ',';
        out += digits[i];
    }
    return out;
}

static void TextFit(ImDrawList* dl, ImVec2 at, float maxX, float size, ImU32 col, const char* text) {
    ImGui::PushFont(nullptr, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, at, ImVec2(maxX, at.y + size * 1.5f), maxX, text, nullptr, nullptr);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Rounded up the way ImGui measures text it may cut short, so it fits
static float TextWidth(float size, const char* text) { return ceilf(ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x) + 1.0f; }

// The item's icon on a dark square (just the square until the icon has been read)
static void IconBox(ImDrawList* dl, int icon, ImVec2 a, float side) {
    ImVec2 b(a.x + side, a.y + side);
    float r = floorf(side * 0.08f), pad = floorf(side * 0.04f);
    dl->AddRectFilled(a, b, Col::ICON_BG, r);
    dl->AddRect(a, b, Col::EDGE, r);
    ItemIcons::Draw(dl, icon, ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, b.y - pad));
}

struct Status {
    std::string text;  // "" for nothing to say
    ImU32 col;
};
static Status StatusOf(const Owned& have, const Got& got, bool full) {
    char b[96];
    if (have.carried && have.stored) snprintf(b, sizeof(b), "You have %d, %d more in your chest", have.carried, have.stored);
    else if (have.carried > 1) snprintf(b, sizeof(b), "You have %d", have.carried);
    else if (have.carried) snprintf(b, sizeof(b), "You have it");
    else if (have.stored > 1) snprintf(b, sizeof(b), "%d in your chest", have.stored);
    else if (have.stored) snprintf(b, sizeof(b), "In your chest");
    else if (!got.handedTo.empty()) return { "Given to " + got.handedTo, Col::GREEN };
    else if (got.picked) return { full ? "Picked up already (it's not in your inventory now)" : "Picked up already", Col::GREEN };
    else return { full ? "You don't have it" : "", Col::DIM };
    std::string text = b;
    if (have.level > 0) text += " (+" + std::to_string(have.level) + ")";  // the best one
    return { text, Col::GREEN };
}

// The items whose name has the text (or every word of it), names starting with it first; only the chosen kind
static void Match(const std::string& q) {
    s_Matches.clear();
    std::vector<std::string> words;
    for (size_t i = 0; i < q.size();) {
        size_t j = q.find(' ', i);
        if (j == std::string::npos) j = q.size();
        if (j > i) words.push_back(q.substr(i, j - i));
        i = j + 1;
    }
    std::vector<int> ranked[3];
    bool available = s_AvailableReady.load();
    std::lock_guard<std::mutex> lock(s_Lock);  // for what the player has
    for (int i = 0; i < (int)s_Items.size(); i++) {
        const Item& it = s_Items[i];
        if (s_Filter >= 0 && it.group != s_Filter) continue;
        // nothing hands it out: only listed when you have it
        if (available && std::none_of(it.keys.begin(), it.keys.end(), [](int64_t k) { return s_Available.count(k) > 0; })) {
            Owned o = OwnedOf(it);
            if (!o.carried && !o.stored) continue;
        }
        if (q.empty() || it.lower.rfind(q, 0) == 0) ranked[0].push_back(i);
        else if (it.lower.find(q) != std::string::npos) ranked[1].push_back(i);
        else if (words.size() > 1 && std::all_of(words.begin(), words.end(), [&](const std::string& w) {
                     return it.lower.find(w) != std::string::npos;
                 }))
            ranked[2].push_back(i);
    }
    for (auto& r : ranked) s_Matches.insert(s_Matches.end(), r.begin(), r.end());
    if (s_Matches.size() > 600) s_Matches.resize(600);
}

static void DrawRow(int i, float rowH, float scale) {
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##row", s_Selected == i, 0, ImVec2(0, rowH))) s_Selected = i;
    Owned have;
    Got got;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        have = OwnedOf(s_Items[i]);
        auto g = s_Got.find(i);
        if (g != s_Got.end()) got = g->second;
    }
    Status st = StatusOf(have, got, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float icon = rowH - floorf(8.0f * scale);
    IconBox(dl, s_Items[i].icon, ImVec2(p.x + floorf(3.0f * scale), p.y + floorf((rowH - icon) * 0.5f)), icon);
    float x = p.x + icon + floorf(14.0f * scale), maxX = p.x + w - floorf(6.0f * scale);
    float nameSize = floorf(20.0f * scale), subSize = floorf(16.0f * scale);
    float y = p.y + floorf((rowH - nameSize - subSize - 3.0f * scale) * 0.5f);
    TextFit(dl, ImVec2(x, y), maxX, nameSize, Col::TEXT, s_Items[i].name.c_str());
    y += nameSize + floorf(3.0f * scale);
    const char* kind = s_Items[i].kind;
    TextFit(dl, ImVec2(x, y), maxX, subSize, Col::FAINT, kind);
    if (!st.text.empty()) {
        float sx = x + TextWidth(subSize, kind);
        TextFit(dl, ImVec2(sx, y), maxX, subSize, Col::FAINT, "  -  ");
        TextFit(dl, ImVec2(sx + TextWidth(subSize, "  -  "), y), maxX, subSize, st.col, st.text.c_str());
    }
    ImGui::PopID();
}

// Search box, kind filter and the matching items
static void DrawList(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w);
    if (s_FocusSearch) {
        ImGui::SetKeyboardFocusHere();
        s_FocusSearch = false;
    }
    ImGui::InputTextWithHint("##find", "Search items, weapons, spells...", s_Query, sizeof(s_Query));
    bool typing = ImGui::IsItemActive();

    ImGui::SetNextItemWidth(floorf(w * 0.55f));
    if (ImGui::BeginCombo("##kind", s_Filter < 0 ? "Everything" : kGroupNames[s_Filter])) {
        if (ImGui::Selectable("Everything", s_Filter < 0)) s_Filter = -1;
        for (int g = 0; g < G_COUNT; g++)
            if (ImGui::Selectable(kGroupNames[g], s_Filter == g)) s_Filter = g;
        ImGui::EndCombo();
    }

    std::string q = s_Query;
    for (char& c : q) c = (char)tolower((unsigned char)c);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    if (q.size() < 2) q.clear();
    bool listing = !q.empty() || s_Filter >= 0;
    if (q != s_MatchedQuery || s_Filter != s_MatchedFilter || s_AvailableReady.load() != s_MatchedAvailable) {
        s_MatchedQuery = q;
        s_MatchedFilter = s_Filter;
        s_MatchedAvailable = s_AvailableReady.load();
        if (listing) Match(q);
        else s_Matches.clear();
        // keep the item picked while it still matches, else show the best match
        if (!s_Matches.empty() && std::find(s_Matches.begin(), s_Matches.end(), s_Selected) == s_Matches.end()) s_Selected = s_Matches[0];
        s_ScrollToSelected = true;
    }
    if (listing) {
        char count[32];
        snprintf(count, sizeof(count), s_Matches.size() >= 600 ? "%d+ found" : s_Matches.size() == 1 ? "%d match" : "%d found",
                 (int)s_Matches.size());
        ImGui::SameLine(w - ImGui::CalcTextSize(count).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::FAINT), "%s", count);
    }

    // up and down arrows pick the next item while typing
    int move = 0;
    if (typing && !s_Matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move = 8;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move = -8;
    }
    if (move) {
        auto it = std::find(s_Matches.begin(), s_Matches.end(), s_Selected);
        int k = it == s_Matches.end() ? 0 : (int)(it - s_Matches.begin()) + move;
        s_Selected = s_Matches[std::clamp(k, 0, (int)s_Matches.size() - 1)];
        s_ScrollToSelected = true;
    }

    float rowH = floorf(50.0f * scale);
    std::vector<int> visible;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 11, 9, 140));
    ImGui::BeginChild("##matches", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    if (!listing) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::DIM);
        ImGui::TextWrapped("Type part of a name, or pick a kind of item to see them all.");
        ImGui::Spacing();
        ImGui::TextWrapped("Up and down arrows move through the list.");
        ImGui::PopStyleColor();
    } else if (s_Matches.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::DIM), "Nothing in the game by that name");
    } else {
        if (s_ScrollToSelected) {
            auto it = std::find(s_Matches.begin(), s_Matches.end(), s_Selected);
            if (it != s_Matches.end()) {
                float y = (float)(it - s_Matches.begin()) * rowH, top = ImGui::GetScrollY();
                float h = ImGui::GetWindowHeight() - style.WindowPadding.y * 2;
                if (y < top) ImGui::SetScrollY(y);
                else if (y + rowH > top + h) ImGui::SetScrollY(y + rowH - h);
            }
            s_ScrollToSelected = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0));
        ImGuiListClipper clip;
        clip.Begin((int)s_Matches.size(), rowH);
        while (clip.Step())
            for (int m = clip.DisplayStart; m < clip.DisplayEnd; m++) {
                visible.push_back(s_Matches[m]);
                DrawRow(s_Matches[m], rowH, scale);
            }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    // the game thread works out what became of the items on screen
    static std::vector<int> s_Listed;
    if (s_Selected >= 0) visible.push_back(s_Selected);
    if (visible != s_Listed) {
        s_Listed = visible;
        std::lock_guard<std::mutex> lock(s_Lock);
        s_Asked = std::move(visible);
        s_AskedVersion++;
    }
}

// The upgrade level, affinity and grip a weapon is shown at (render thread)
static int s_StatItem = -1, s_StatLevel = 0, s_StatAffinity = 0;
static bool s_StatTwoHand = false;

// The ranking's choices (render thread)
static SuggestAsk s_Suggest;
static int s_SuggestPicked = -1;  // the row last clicked

// Pick a weapon from the ranking: the details show it as ranked
static void PickSuggestion(const Suggestion& s, const SuggestView& v) {
    s_Selected = s.item;
    s_StatItem = s.item;
    s_StatLevel = s.level;
    s_StatAffinity = s.affinity;
    s_StatTwoHand = v.ask.twoHand && s.item2 < 0;
}

static void DrawSuggestions(float scale) {
    s_SuggestShownAt = GetTickCount64();
    SuggestView v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_SuggestAsk = s_Suggest;
        v = s_SuggestView;
    }
    float w = ImGui::GetContentRegionAvail().x;
    const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(Col::DIM);

    // what to rank, and how
    ImGui::SetNextItemWidth(w);
    const char* current = s_Suggest.filter < kGroupCount ? kFilterNames[s_Suggest.filter] : kClasses[s_Suggest.filter - kGroupCount].name;
    if (ImGui::BeginCombo("##rank", current, ImGuiComboFlags_HeightLarge)) {
        for (int f = 0; f < kGroupCount + kClassCount; f++) {
            if (f == kGroupCount) ImGui::Separator();
            const char* name = f < kGroupCount ? kFilterNames[f] : kClasses[f - kGroupCount].name;
            if (ImGui::Selectable(name, s_Suggest.filter == f)) s_Suggest.filter = f;
        }
        ImGui::EndCombo();
    }
    bool melee = MeleeFilter(s_Suggest.filter);
    bool combos = s_IdxState.load() == 2 && !s_Idx.combos.empty();
    int level = s_Suggest.level >= 0 ? s_Suggest.level : v.yourLevel;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(dim, "Upgraded to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(floorf(w * 0.32f));
    if (ImGui::SliderInt("##at", &level, 0, 25, "+%d", ImGuiSliderFlags_AlwaysClamp)) s_Suggest.level = level;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Somber weapons are compared at the level that counts the same (+10 for +25).\n"
                          "It starts at the best upgrade you have now.");
    if (s_Suggest.level >= 0 && s_Suggest.level != v.yourLevel) {
        ImGui::SameLine();
        if (ImGui::SmallButton("yours")) s_Suggest.level = -1;
    }
    ImGui::BeginDisabled(!melee);
    ImGui::Checkbox("Dual wield", &s_Suggest.dual);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Powerstance: two weapons of the same class, one in each hand.\nThe best two of each class, added up.");
    ImGui::SameLine();
    ImGui::BeginDisabled(s_Suggest.dual && melee);
    ImGui::Checkbox("Two-handed", &s_Suggest.twoHand);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!melee || !combos);
    ImGui::Checkbox("Damage per second", &s_Suggest.perSecond);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(combos ? "Ranks by light-attack damage a second instead of attack power: a round of the weapon's\n"
                                   "R1 combo from the pack's animations (each hit's motion value times attack power, attacking\n"
                                   "again at the first chance), assuming every hit lands. Enemy defence, stamina, reach\n"
                                   "and bleed or frost buildup aren't counted."
                                 : "DropFinder.txt has no combos in it: it needs rebuilding (er-drop-finder/build.py).");
    ImGui::SameLine();
    ImGui::Checkbox("Only ones you can get", &s_Suggest.onlyAvailable);

    if (!(v.ask == s_Suggest)) {
        ImGui::TextColored(dim, "Ranking...");
        return;
    }
    if (!v.on) {
        ImGui::TextColored(dim, "Couldn't read your attributes");
        return;
    }
    bool spells = v.ask.filter == F_SORCERY || v.ask.filter == F_INCANTATION;
    const char* unit = spells ? (v.ask.filter == F_SORCERY ? "sorcery scaling" : "incantation scaling") : v.ask.dual && melee ? "combined" : "attack";
    if (v.yours.item >= 0) {
        std::string text = "Your best now: " + v.yours.label + "  -  " +
                           (v.perSecond ? Grouped(v.yours.rate) + " damage/s" : std::to_string((int)v.yours.value) + " " + unit);
        ImVec2 p = ImGui::GetCursorScreenPos();
        TextFit(ImGui::GetWindowDrawList(), p, p.x + w, ImGui::GetFontSize(), Col::GREEN, text.c_str());
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
    }

    float rowH = floorf(50.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 11, 9, 140));
    ImGui::BeginChild("##ranked", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    if (v.list.empty()) ImGui::TextColored(dim, "Nothing to rank here");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    ImGuiListClipper clip;
    clip.Begin((int)v.list.size(), rowH);
    while (clip.Step())
        for (int n = clip.DisplayStart; n < clip.DisplayEnd; n++) {
            const Suggestion& sg = v.list[n];
            ImGui::PushID(n);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##row", s_SuggestPicked == n && s_Selected == sg.item, 0, ImVec2(0, rowH))) {
                s_SuggestPicked = n;
                PickSuggestion(sg, v);
            }
            if (v.perSecond && sg.seconds > 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s damage a second: %d hits in a %.1f s round of light attacks%s", sg.label.c_str(),
                                  Grouped(sg.rate).c_str(), sg.hits + sg.leftHits, sg.seconds,
                                  sg.item2 >= 0 ? "\n(the first in your right hand, the second in your left)" : "");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float icon = rowH - floorf(8.0f * scale);
            IconBox(dl, s_Items[sg.item].icon, ImVec2(p.x + floorf(3.0f * scale), p.y + floorf((rowH - icon) * 0.5f)), icon);
            float x = p.x + icon + floorf(14.0f * scale), maxX = p.x + rw - floorf(6.0f * scale);
            float nameSize = floorf(20.0f * scale), subSize = floorf(16.0f * scale);
            float y = p.y + floorf((rowH - nameSize - subSize - 3.0f * scale) * 0.5f);
            char rank[16];
            snprintf(rank, sizeof(rank), "%d.  ", n + 1);
            TextFit(dl, ImVec2(x, y), maxX, nameSize, Col::FAINT, rank);
            float rx = x + TextWidth(nameSize, rank);
            TextFit(dl, ImVec2(rx, y), maxX, nameSize, Col::TEXT, sg.label.c_str());
            y += nameSize + floorf(3.0f * scale);
            char sub[160];
            const WeaponClass* c = sg.cls >= 0 ? &kClasses[sg.cls] : nullptr;
            const char* kind = c ? c->name : "Weapon";
            if (v.perSecond) snprintf(sub, sizeof(sub), "%s%s  -  %s damage/s  -  %d %s", kind, sg.item2 >= 0 ? " pair" : "",
                                      Grouped(sg.rate).c_str(), (int)sg.value, unit);
            else snprintf(sub, sizeof(sub), "%s%s  -  %d %s", kind, sg.item2 >= 0 ? " pair" : "", (int)sg.value, unit);
            TextFit(dl, ImVec2(rx, y), maxX, subSize, Col::FAINT, sub);
            if (sg.owned) {
                float sx = rx + TextWidth(subSize, sub);
                TextFit(dl, ImVec2(sx, y), maxX, subSize, Col::GREEN, sg.item2 >= 0 ? "  -  you have both" : "  -  you have it");
            }
            ImGui::PopID();
        }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// One place the item comes from, as a card
static void DrawSource(const Source& s, int n, float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    bool tracked;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        tracked = s_Track.on && s_Track.pos.space == s.pos.space && s_Track.pos.x == s.pos.x && s_Track.pos.z == s.pos.z &&
                  s_Track.label == s_Items[s_Selected].name + ": " + s.label;
    }
    ImGui::PushID(n);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, s.done ? Col::CARD_DONE : Col::CARD);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(floorf(12.0f * scale), floorf(8.0f * scale)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, floorf(4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, floorf(3.0f * scale)));
    ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // what it is, and on the right Track (or that you got it)
    const char* right = s.done ? (s.sure ? "You got this one" : "Looted already") : nullptr;
    const char* button = !s.done && s.placed ? (tracked ? "Tracking" : "Track") : nullptr;
    float rw = button ? ImGui::CalcTextSize(button).x + style.FramePadding.x * 2 : right ? ImGui::CalcTextSize(right).x : 0.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize();
    TextFit(dl, ImVec2(p.x, p.y + style.FramePadding.y), p.x + w - rw - floorf(12.0f * scale), fs, s.done ? Col::DIM : Col::BRIGHT, s.what.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - rw, p.y));
    if (button) {
        if (tracked) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(96, 84, 52, 255));
        if (ImGui::Button(button)) {
            std::lock_guard<std::mutex> lock(s_Lock);
            if (tracked) s_Track.on = false;
            else s_Track = { true, s.pos, s_Items[s_Selected].name + ": " + s.label };
        }
        if (tracked) ImGui::PopStyleColor();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(right ? Col::GREEN : Col::DIM), "%s", right ? right : "");
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Col::DIM);
    if (!s.note.empty()) ImGui::TextWrapped("%s", s.note.c_str());
    if (!s.where.empty()) {
        std::string where = s.where;
        if (s.dist >= 0) where += "  -  " + Metres(s.dist) + " from you";
        ImGui::TextWrapped("%s", where.c_str());
    }
    ImGui::PopStyleColor();
    if (!s.route.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, !s.done && s.route.rfind("Warp", 0) == 0 ? Col::GREEN : Col::DIM);
        ImGui::TextWrapped("%s", s.route.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

static const char* Grade(float scaling) {
    return scaling >= 175 ? "S" : scaling >= 140 ? "A" : scaling >= 90 ? "B" : scaling >= 60 ? "C" : scaling >= 25 ? "D" : "E";
}

// Attack power with your attributes, at an upgrade level and affinity you pick, and what it scales with
static void DrawWeapon(const Owned& have, float scale) {
    if (s_StatItem != s_Selected) {  // start at the best one you have
        s_StatItem = s_Selected;
        s_StatLevel = (std::max)(have.level, 0);
        s_StatAffinity = have.level >= 0 ? have.affinity : 0;
    }
    WeaponView v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_WeaponAsk = { s_Selected, s_StatLevel, s_StatAffinity, s_StatTwoHand };
        v = s_WeaponView;
    }
    static const char* const kDamage[5] = { "Physical", "Magic", "Fire", "Lightning", "Holy" };
    static const char* const kAttr[5] = { "Str", "Dex", "Int", "Fai", "Arc" };
    const ImVec4 text = ImGui::ColorConvertU32ToFloat4(Col::TEXT), dim = ImGui::ColorConvertU32ToFloat4(Col::DIM),
                 faint = ImGui::ColorConvertU32ToFloat4(Col::FAINT), red(0.85f, 0.45f, 0.36f, 1.0f),
                 gold = ImGui::ColorConvertU32ToFloat4(Col::GOLD);

    ImGui::Spacing();
    ImGui::TextColored(gold, "Attack power");
    if (v.on && v.ask.item == s_Selected) {
        ImGui::SameLine();
        ImGui::TextColored(faint, v.statsKnown ? "with your attributes" : "base only (couldn't read your attributes)");
    }
    ImGui::Separator();
    if (!v.on || v.ask.item != s_Selected) {
        ImGui::TextColored(dim, "Working it out...");
        return;
    }
    float w = ImGui::GetContentRegionAvail().x;
    if (v.maxLevel > 0) {
        ImGui::SetNextItemWidth(floorf(w * 0.34f));
        ImGui::SliderInt("##level", &s_StatLevel, 0, v.maxLevel, "+%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
    }
    if (v.affinities.size() > 1) {
        const char* current = "Standard";
        for (const auto& a : v.affinities)
            if (a.first == v.affinity) current = a.second.c_str();
        ImGui::SetNextItemWidth(floorf(w * 0.30f));
        if (ImGui::BeginCombo("##affinity", current)) {
            for (const auto& a : v.affinities)
                if (ImGui::Selectable(a.second.c_str(), a.first == v.affinity)) s_StatAffinity = a.first;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::Checkbox("Two-handed", &s_StatTwoHand);

    // damage and buildup, two to a line
    struct Cell {
        const char* name;
        float base, bonus;
    };
    std::vector<Cell> cells;
    float total = 0;
    for (int t = 0; t < 5; t++)
        if (v.base[t] > 0) {
            cells.push_back({ kDamage[t], v.base[t], v.bonus[t] });
            total += floorf(v.base[t]) + floorf(v.bonus[t]);
        }
    for (const Buildup& b : v.buildup) cells.push_back({ b.name, b.base, b.bonus });
    if (ImGui::BeginTable("##attack", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (size_t i = 0; i < cells.size(); i++) {
            const Cell& c = cells[i];
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "%s", c.name);
            ImGui::TableNextColumn();
            ImGui::TextColored(text, "%.0f", floorf(c.base) + floorf(c.bonus));
            if (c.bonus != 0) {
                ImGui::SameLine();
                ImGui::TextColored(c.bonus > 0 ? faint : red, "(%.0f %s %.0f)", floorf(c.base), c.bonus > 0 ? "+" : "-", fabsf(floorf(c.bonus)));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(dim, "Total");
    ImGui::SameLine();
    ImGui::TextColored(text, "%.0f", total);
    ImGui::SameLine(0, floorf(24.0f * scale));
    ImGui::TextColored(dim, "Critical");
    ImGui::SameLine();
    ImGui::TextColored(text, "%d", v.critical);
    for (int k = 0; k < 2; k++) {
        float spell = k ? v.incantation : v.sorcery;
        if (spell < 0) continue;
        ImGui::SameLine(0, floorf(24.0f * scale));
        ImGui::TextColored(dim, k ? "Incantation scaling" : "Sorcery scaling");
        ImGui::SameLine();
        ImGui::TextColored(spell < 100.0f ? red : text, "%.0f", floorf(spell));
    }

    // light-attack damage a second, as held and powerstanced
    for (int k = 0; k < 2; k++) {
        float rate = k ? v.dualRate : v.rate, seconds = k ? v.dualSeconds : v.seconds;
        if (rate <= 0) continue;
        ImGui::TextColored(dim, k ? "Powerstanced" : "Light attacks");
        ImGui::SameLine();
        ImGui::TextColored(text, "%s damage/s", Grouped(rate).c_str());
        ImGui::SameLine();
        ImGui::TextColored(faint, "%s%d hits in %.1f s", k ? "two of these: " : "", k ? v.dualHits : v.hits, seconds);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A round of the %s R1 combo from the pack's animations: each hit's motion value\n"
                              "times this attack power, attacking again at the first chance and every hit landing.\n"
                              "Enemy defence, stamina, reach and buildup aren't counted.",
                              k ? "powerstance (one in each hand, one-handed attributes)" : v.ask.twoHand ? "two-handed" : "one-handed");
    }

    // what it scales with, what it needs, what you have
    ImGui::TextColored(dim, "Scaling");
    bool any = false;
    for (int a = 0; a < 5; a++) {
        if (v.scaling[a] < 1.0f) continue;
        ImGui::SameLine(0, floorf((any ? 16.0f : 10.0f) * scale));
        ImGui::TextColored(text, "%s %s", kAttr[a], Grade(v.scaling[a]));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s scaling %.0f", kAttr[a], v.scaling[a]);
        any = true;
    }
    if (!any) {
        ImGui::SameLine();
        ImGui::TextColored(faint, "none");
    }
    ImGui::TextColored(dim, "Requires");
    any = false;
    for (int a = 0; a < 5; a++) {
        if (!v.need[a]) continue;
        ImGui::SameLine(0, floorf((any ? 16.0f : 10.0f) * scale));
        ImGui::TextColored(v.statsKnown && v.have[a] < v.need[a] ? red : text, "%s %d", kAttr[a], v.need[a]);
        any = true;
    }
    if (!any) {
        ImGui::SameLine();
        ImGui::TextColored(faint, "nothing");
    }
    if (v.statsKnown) {
        ImGui::TextColored(dim, "You have");
        for (int a = 0; a < 5; a++) {
            ImGui::SameLine(0, floorf((a ? 16.0f : 10.0f) * scale));
            ImGui::TextColored(faint, "%s %d", kAttr[a], v.have[a]);
        }
        if (s_StatTwoHand) {
            ImGui::SameLine();
            ImGui::TextColored(faint, "(Str x1.5 two-handed)");
        }
    }
}

// Damage negation, resistances, poise and weight of a piece of armor
static void DrawArmor() {
    ArmorView v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        v = s_ArmorView;
    }
    if (!v.on || v.item != s_Selected) return;
    static const char* const kNegation[8] = { "Physical", "Strike", "Slash", "Pierce", "Magic", "Fire", "Lightning", "Holy" };
    static const char* const kResist[4] = { "Immunity", "Robustness", "Focus", "Vitality" };
    const ImVec4 text = ImGui::ColorConvertU32ToFloat4(Col::TEXT), dim = ImGui::ColorConvertU32ToFloat4(Col::DIM),
                 gold = ImGui::ColorConvertU32ToFloat4(Col::GOLD);
    ImGui::Spacing();
    ImGui::TextColored(gold, "Damage negation");
    ImGui::Separator();
    if (ImGui::BeginTable("##negation", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < 8; i++) {
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "%s", kNegation[i]);
            ImGui::TableNextColumn();
            ImGui::TextColored(text, "%.1f", v.negation[i]);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextColored(gold, "Resistance");
    ImGui::Separator();
    if (ImGui::BeginTable("##resist", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < 4; i++) {
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "%s", kResist[i]);
            ImGui::TableNextColumn();
            ImGui::TextColored(text, "%d", v.resist[i]);
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(dim, "Poise");
        ImGui::TableNextColumn();
        ImGui::TextColored(text, "%.1f", v.poise);
        ImGui::TableNextColumn();
        ImGui::TextColored(dim, "Weight");
        ImGui::TableNextColumn();
        ImGui::TextColored(text, "%.1f", v.weight);
        ImGui::EndTable();
    }
}

// The picked item: what it is, whether you have it, and where to get it
static void DrawDetails(float scale) {
    if (s_Selected < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::DIM);
        ImGui::TextWrapped("Pick an item to see where the game hands it out, the nearest site of grace and the closest one you can warp to.");
        ImGui::PopStyleColor();
        return;
    }
    const Item& item = s_Items[s_Selected];
    std::vector<Source> res;
    int resItem;
    Owned have;
    Got got;
    std::string info, caption;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        res = s_Results;
        resItem = s_ResultItem;
        have = OwnedOf(item);
        auto g = s_Got.find(s_Selected);
        if (g != s_Got.end()) got = g->second;
        if (resItem == s_Selected) info = s_ResultInfo, caption = s_ResultCaption;
    }
    bool ready = resItem == s_Selected;

    // icon, name, kind and what you have
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, side = floorf(96.0f * scale);
    IconBox(dl, item.icon, p, side);
    float x = p.x + side + floorf(16.0f * scale), title = floorf(28.0f * scale), subSize = floorf(17.0f * scale);
    TextFit(dl, ImVec2(x, p.y), p.x + w, title, Col::BRIGHT, item.name.c_str());
    TextFit(dl, ImVec2(x, p.y + title + floorf(4.0f * scale)), p.x + w, subSize, Col::DIM, item.kind);
    Status st = StatusOf(have, got, true);
    float pillY = p.y + title + subSize + floorf(14.0f * scale), padX = floorf(8.0f * scale), padY = floorf(3.0f * scale);
    ImVec2 a(x, pillY), b(x + (std::min)(TextWidth(subSize, st.text.c_str()) + padX * 2, p.x + w - x), pillY + subSize + padY * 2);
    dl->AddRectFilled(a, b, (st.col & 0x00ffffff) | 0x26000000, floorf(4.0f * scale));
    dl->AddRect(a, b, (st.col & 0x00ffffff) | 0x80000000, floorf(4.0f * scale));
    TextFit(dl, ImVec2(a.x + padX, a.y + padY), b.x - padX, subSize, st.col, st.text.c_str());
    ImGui::Dummy(ImVec2(side, side));
    if (ImGui::IsItemHovered() && !caption.empty()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(floorf(420.0f * scale));
        ImGui::TextUnformatted(caption.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::DIM);
    if (!info.empty()) ImGui::TextWrapped("%s", info.c_str());
    if (!caption.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::GOLD);
        if (ImGui::SmallButton(s_ShowCaption ? "Hide the description" : "Description")) s_ShowCaption = !s_ShowCaption;
        ImGui::PopStyleColor();
        if (s_ShowCaption) ImGui::TextWrapped("%s", caption.c_str());
    }
    bool anyDone = got.picked || !got.handedTo.empty();
    for (const Source& s : res) anyDone = anyDone || s.done;
    if (s_Guest && anyDone) ImGui::TextWrapped("You're in the host's world: what's marked as got can be what the host got.");
    ImGui::PopStyleColor();
    if (item.group == G_WEAPON) DrawWeapon(have, scale);
    if (item.group == G_ARMOR) DrawArmor();

    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::GOLD), "Where to get it");
    if (ready && !res.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::FAINT), "%d %s", (int)res.size(), res.size() == 1 ? "place" : "places");
    }
    ImGui::Separator();
    if (!ready) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::DIM), "Looking it up...");
        return;
    }
    if (res.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::DIM);
        ImGui::TextWrapped("The game's tables don't hand this out anywhere: a starting item, crafted, or from a script this can't read.");
        ImGui::PopStyleColor();
        return;
    }
    for (size_t i = 0; i < res.size(); i++) DrawSource(res[i], (int)i, scale);
}

static void DrawFinder(float scale) {
    if (s_IdxState.load() == 0) {
        s_IdxState = 1;
        std::thread(ReadIndex).detach();
    }
    if (s_ItemsState.load() == 0) s_ItemsState = 1;
    if (s_IdxState.load() == 3) {
        ImGui::TextUnformatted(s_IdxError.c_str());
        return;
    }
    if (s_ItemsState.load() != 2 || s_IdxState.load() != 2) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Col::DIM),
                           s_ItemsState.load() == 3 ? "Couldn't read the game's item names" : "Reading the game's items... %d%%", s_ItemsProgress.load());
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    bool tracking;
    std::string trackLabel;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        tracking = s_Track.on;
        trackLabel = s_Track.label;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float footer = tracking ? ImGui::GetFrameHeight() + style.ItemSpacing.y * 2 + 1.0f : 0.0f;
    float left = floorf((std::max)(300.0f * scale, avail.x * 0.42f)), h = avail.y - footer;
    ImGui::BeginChild("##left", ImVec2(left, h));
    if (ImGui::BeginTabBar("##mode")) {
        if (ImGui::BeginTabItem("Search")) {
            DrawList(scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Best weapons for you")) {
            DrawSuggestions(scale);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, floorf(16.0f * scale));
    ImGui::BeginChild("##right", ImVec2(0, h));
    DrawDetails(scale);
    ImGui::EndChild();
    s_WantItem = s_Selected;

    if (tracking) {
        ImGui::Separator();
        const char* stop = "Stop tracking";
        float bw = ImGui::CalcTextSize(stop).x + style.FramePadding.x * 2;
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string text = "Tracking  " + trackLabel;
        TextFit(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + style.FramePadding.y), p.x + avail.x - bw - floorf(12.0f * scale),
                ImGui::GetFontSize(), Col::DIM, text.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + avail.x - bw, p.y));
        if (ImGui::Button(stop)) StopTracking();
    }
}

void DrawWindow(float scale, ImVec2 nextTo) {
    if (!s_WindowOpen) return;
    s_ShownAt = GetTickCount64();
    ItemIcons::Update();
    auto& io = ImGui::GetIO();
    int frame = ImGui::GetFrameCount();
    if (frame - s_LastWindowFrame > 1) {
        s_PlaceFrames = 2;
        s_FocusSearch = true;
        s_ScrollToSelected = true;
    }
    s_LastWindowFrame = frame;
    float margin = 16.0f * scale;
    ImVec2 size(WindowWidth(scale), (std::min)(kWindowH * scale, io.DisplaySize.y - margin * 2));
    if (s_PlaceFrames > 0) {
        // next to the menu, which moves over to make room on the frame after the finder opens
        s_PlaceFrames--;
        ImVec2 pos((std::max)(margin, (std::min)(nextTo.x, io.DisplaySize.x - size.x - margin)),
                   (std::max)(margin, (std::min)(nextTo.y, io.DisplaySize.y - size.y - margin)));
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f * scale, 400.0f * scale), ImVec2(FLT_MAX, FLT_MAX));
    static const std::pair<ImGuiCol, ImU32> kColors[] = {
        { ImGuiCol_FrameBg, IM_COL32(36, 33, 26, 255) },        { ImGuiCol_FrameBgHovered, IM_COL32(46, 42, 33, 255) },
        { ImGuiCol_FrameBgActive, IM_COL32(52, 47, 37, 255) },  { ImGuiCol_Header, IM_COL32(80, 70, 46, 255) },
        { ImGuiCol_HeaderHovered, IM_COL32(52, 47, 36, 255) },  { ImGuiCol_HeaderActive, IM_COL32(96, 84, 56, 255) },
        { ImGuiCol_PopupBg, IM_COL32(20, 18, 14, 250) },        { ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0) },
        { ImGuiCol_ScrollbarGrab, IM_COL32(80, 72, 56, 255) },  { ImGuiCol_ScrollbarGrabHovered, IM_COL32(104, 94, 72, 255) },
        { ImGuiCol_ScrollbarGrabActive, IM_COL32(124, 112, 86, 255) }, { ImGuiCol_TextSelectedBg, IM_COL32(120, 104, 64, 160) },
        { ImGuiCol_ResizeGrip, IM_COL32(78, 71, 55, 160) },     { ImGuiCol_ResizeGripHovered, IM_COL32(120, 108, 84, 220) },
        { ImGuiCol_ResizeGripActive, IM_COL32(146, 139, 108, 255) }, { ImGuiCol_NavCursor, IM_COL32(181, 173, 133, 200) },
        { ImGuiCol_Tab, IM_COL32(36, 33, 26, 255) },            { ImGuiCol_TabHovered, IM_COL32(70, 62, 44, 255) },
        { ImGuiCol_TabSelected, IM_COL32(80, 70, 46, 255) },    { ImGuiCol_TabSelectedOverline, IM_COL32(181, 173, 133, 255) },
        { ImGuiCol_TabDimmed, IM_COL32(30, 27, 21, 255) },      { ImGuiCol_TabDimmedSelected, IM_COL32(64, 56, 38, 255) },
        { ImGuiCol_CheckMark, IM_COL32(200, 186, 140, 255) },   { ImGuiCol_SliderGrab, IM_COL32(150, 136, 96, 255) },
        { ImGuiCol_SliderGrabActive, IM_COL32(190, 172, 122, 255) },
    };
    for (const auto& c : kColors) ImGui::PushStyleColor(c.first, c.second);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, floorf(3.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(floorf(8.0f * scale), floorf(5.0f * scale)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, floorf(12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, floorf(3.0f * scale));
    if (ImGui::Begin("Find an item", &s_WindowOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushStyleColor(ImGuiCol_Border, Col::EDGE);
        DrawFinder(scale);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor((int)(sizeof(kColors) / sizeof(kColors[0])));
}

std::string Tracking() {
    std::lock_guard<std::mutex> lock(s_Lock);
    return s_Track.on ? s_Track.label : std::string();
}

bool TrackedSpot(WPos& at, std::string& label) {
    std::lock_guard<std::mutex> lock(s_Lock);
    if (!s_Track.on) return false;
    at = s_Track.pos;
    label = s_Track.label;
    return true;
}

void StopTracking() {
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Track.on = false;
}

// ---- HUD ----

void DrawTracker(ImFont* font) {
    TrackView v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        v = s_TrackView;
    }
    if (!v.on) return;
    auto& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float fs = floorf(20.0f * scale);
    ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.115f);
    char text[256];
    if (!v.same) snprintf(text, sizeof(text), "%s  -  in another area", v.label.c_str());
    else if (v.dist < 6.0f && fabsf(v.dy) < 6.0f) snprintf(text, sizeof(text), "%s  -  here", v.label.c_str());
    else {
        std::string d = Metres(v.dist);
        if (fabsf(v.dy) > 8.0f) d += ", " + Metres(fabsf(v.dy)) + (v.dy > 0 ? " up" : " down");
        snprintf(text, sizeof(text), "%s  -  %s", v.label.c_str(), d.c_str());
    }
    ImFont* f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    float ty = c.y + 22.0f * scale;
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f + 1, ty + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, ty), IM_COL32(226, 214, 176, 235), text);
    if (!v.same || !v.haveHeading || v.dist < 6.0f) return;
    float a = v.rel / 57.29578f, s = sinf(a), co = cosf(a);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * co - y * s, c.y + x * s + y * co); };
    float r = 16.0f * scale;
    ImVec2 tip = P(0, -r), l = P(-r * 0.62f, r * 0.7f), mid = P(0, r * 0.25f), rt = P(r * 0.62f, r * 0.7f);
    dl->AddTriangleFilled(tip, l, mid, IM_COL32(226, 214, 176, 235));
    dl->AddTriangleFilled(tip, mid, rt, IM_COL32(190, 176, 132, 235));
    dl->AddPolyline(std::vector<ImVec2>{ tip, l, mid, rt }.data(), 4, IM_COL32(20, 18, 14, 220), ImDrawFlags_Closed, 1.5f * scale);
}

} // namespace DropFinder
