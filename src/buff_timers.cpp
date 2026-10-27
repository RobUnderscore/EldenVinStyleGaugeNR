#include "buff_timers.h"
#include "memory.h"
#include "status_icons.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

extern void Log(const char* fmt, ...);

namespace BuffTimers {

// SpEffect list node (Elden Ring): +0x00 -> its SpEffectParam row, +0x08 param id,
// remaining time at Game()->nodeRemaining. SpEffectParam row: +0x00 iconId, +0x08 effectEndurance.
static constexpr ptrdiff_t NODE_PARAM = 0x00;
static constexpr ptrdiff_t NODE_ID = 0x08;
static constexpr ptrdiff_t PARAM_ICON = 0x00;
static constexpr ptrdiff_t PARAM_ENDURANCE = 0x08;

static std::vector<Buff> s_Buffs;
static std::vector<int32_t> s_Order;                   // icon keys (see Key), first seen first
static std::unordered_map<int32_t, float> s_Peak;      // highest remaining seen per icon
static std::unordered_set<int32_t> s_Logged;           // effect ids already written to the log

bool IconRect(int32_t iconId, int& sheet, float& x, float& y, float& w, float& h) {
    auto end = std::end(kStatusIcons);
    auto it = std::lower_bound(std::begin(kStatusIcons), end, iconId,
        [](const StatusIcon& s, int32_t id) { return s.id < id; });
    if (it == end || it->id != iconId) return false;
    sheet = it->sheet; x = it->x; y = it->y; w = it->w; h = it->h;
    return true;
}

// Weapon buff -> cell in BuffIcons.dds (-1 = the game's sword status icon); false if not a weapon buff
static bool WeaponBuffCell(int32_t effectId, int16_t& cell) {
    auto end = std::end(kWeaponBuffIcons);
    auto it = std::lower_bound(std::begin(kWeaponBuffIcons), end, effectId,
        [](const WeaponBuffIcon& w, int32_t id) { return w.effectId < id; });
    if (it == end || it->effectId != effectId) return false;
    cell = it->cell;
    return true;
}

// Buffs are merged per icon: status icons by id, item icons by cell
static int32_t Key(const Buff& b) { return b.cell >= 0 ? -1 - b.cell : b.iconId; }

const char* ItemIconsFile() { return "BuffIcons.dds"; }

int SheetCount() { return (int)(sizeof(kStatusSheets) / sizeof(kStatusSheets[0])); }
const wchar_t* SheetName(int sheet) { return kStatusSheets[sheet]; }

const std::vector<Buff>& Update(float minDuration) {
    s_Buffs.clear();
    const Memory::GameLayout* game = Memory::Game();
    uintptr_t head = game ? Memory::GetSpEffectListHead() : 0;
    if (!head) {
        s_Order.clear();
        s_Peak.clear();
        return s_Buffs;
    }

    std::unordered_map<int32_t, Buff> byIcon;  // by Key()
    uintptr_t node = head;
    int count = 0;
    do {
        count++;
        int32_t id = Memory::Read<int32_t>(node + NODE_ID);
        uintptr_t row = Memory::Read<uintptr_t>(node + NODE_PARAM);
        float remaining = Memory::Read<float>(node + game->nodeRemaining);
        int32_t icon = row ? Memory::Read<int32_t>(row + PARAM_ICON) : -1;
        float endurance = row ? Memory::Read<float>(row + PARAM_ENDURANCE) : 0.0f;
        float nodeTotal = Memory::Read<float>(node + game->nodeRemaining + 8);

        if (s_Logged.size() < 400 && s_Logged.insert(id).second)
            Log("BuffTimers: effect %d row=%llX icon=%d endurance=%.1f remaining=%.1f node+8=%.1f",
                id, (unsigned long long)row, icon, endurance, remaining, nodeTotal);

        int sheet; float sx, sy, sw, sh;
        int16_t cell = -1;
        bool shown = icon > 0 && IconRect(icon, sheet, sx, sy, sw, sh);
        if (!shown && WeaponBuffCell(id, cell)) {
            shown = true;
            if (cell < 0) icon = kWeaponBuffFallbackIcon;
        }
        if (shown && remaining > 0.0f) {
            float total = remaining;
            if (endurance > total) total = endurance;
            if (nodeTotal > total && nodeTotal < 100000.0f) total = nodeTotal;
            Buff b{ icon, remaining, total, cell };
            auto it = byIcon.find(Key(b));
            if (it == byIcon.end() || remaining > it->second.remaining)
                byIcon[Key(b)] = b;
        }

        uintptr_t next = Memory::Read<uintptr_t>(node + game->nodeNext);
        if (next == 0 || next == head || next == node || count > 500) break;
        node = next;
    } while (true);

    // Refreshing a buff restarts its bar: remember the highest remaining per icon
    for (auto& [key, b] : byIcon) {
        float& peak = s_Peak[key];
        if (b.remaining > peak + 0.5f) peak = b.remaining;
        if (peak > b.total) b.total = peak;
    }
    for (auto it = s_Peak.begin(); it != s_Peak.end();)
        it = byIcon.count(it->first) ? std::next(it) : s_Peak.erase(it);

    s_Order.erase(std::remove_if(s_Order.begin(), s_Order.end(),
        [&](int32_t key) { return !byIcon.count(key); }), s_Order.end());
    for (auto& [key, b] : byIcon)
        if (std::find(s_Order.begin(), s_Order.end(), key) == s_Order.end()) s_Order.push_back(key);

    for (int32_t key : s_Order) {
        const Buff& b = byIcon[key];
        if (b.total >= minDuration) s_Buffs.push_back(b);
    }
    return s_Buffs;
}

static ImU32 WithAlpha(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, (int)(255.0f * std::clamp(a, 0.0f, 1.0f)));
}

// Parchment while there's time, amber under 10 s, ember red under 5 s
static void TimeColor(float remaining, int& r, int& g, int& b) {
    if (remaining >= 10.0f) { r = 226; g = 214; b = 176; }
    else if (remaining >= 5.0f) { r = 236; g = 168; b = 82; }
    else { r = 228; g = 86; b = 58; }
}

static void FormatTime(float s, char* out, size_t n) {
    if (s >= 3600.0f) snprintf(out, n, "%dh", (int)(s / 3600.0f));
    else if (s >= 60.0f) snprintf(out, n, "%d:%02d", (int)s / 60, (int)s % 60);
    else if (s >= 10.0f) snprintf(out, n, "%d", (int)s);
    else snprintf(out, n, "%.1f", s);
}

struct Metrics { float iconSize, cellW, barH, textSize, cellH; };

static Metrics GetMetrics(float scale) {
    Metrics m;
    m.iconSize = floorf(34.0f * scale);
    m.cellW = m.iconSize + floorf(10.0f * scale);
    m.barH = std::max(2.0f, floorf(3.0f * scale));
    m.textSize = floorf(18.0f * scale);
    m.cellH = m.iconSize + m.barH + m.textSize + floorf(12.0f * scale);
    return m;
}

ImVec2 Measure(size_t count, float scale, int perRow) {
    if (perRow < 1) perRow = 1;
    Metrics m = GetMetrics(scale);
    size_t cols = std::min(count, (size_t)perRow), rows = (count + perRow - 1) / perRow;
    return ImVec2(cols * m.cellW, rows * m.cellH);
}

std::vector<Buff> Preview() {
    // a spread of icons, with times picked to show each colour
    const float times[] = { 154.0f, 42.0f, 8.3f, 3.1f };
    std::vector<Buff> out;
    size_t n = sizeof(kStatusIcons) / sizeof(kStatusIcons[0]);
    for (int i = 0; i < 4 && n; i++)
        out.push_back(Buff{ kStatusIcons[(i * 37 + 5) % n].id, times[i], 180.0f });
    return out;
}

void Draw(ImDrawList* draw, ImFont* font, const Sheets& sheets, const std::vector<Buff>& buffs,
          float x, float y, float scale, int perRow, float opacity) {
    if (!font) font = ImGui::GetFont();
    if (perRow < 1) perRow = 1;
    const Metrics m = GetMetrics(scale);
    const float iconSize = m.iconSize, cellW = m.cellW, barH = m.barH, textSize = m.textSize, cellH = m.cellH;
    const float t = (float)ImGui::GetTime();

    for (size_t i = 0; i < buffs.size(); i++) {
        const Buff& b = buffs[i];
        float cx = floorf(x + (i % perRow) * cellW);
        float cy = floorf(y + (i / perRow) * cellH);
        float frac = b.total > 0.0f ? std::clamp(b.remaining / b.total, 0.0f, 1.0f) : 1.0f;
        float pulse = b.remaining < 5.0f ? 0.55f + 0.45f * fabsf(sinf(t * 5.0f)) : 1.0f;

        int sheet; float sx, sy, sw, sh;
        if (b.cell >= 0 && sheets.items) {
            // the game has no status icon for weapon buffs: the item's icon on a dark tile
            // with a thin trim, roughly the look of the status icons' frames
            draw->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + iconSize, cy + iconSize), WithAlpha(20, 18, 15, opacity * 0.85f));
            draw->AddRect(ImVec2(cx, cy), ImVec2(cx + iconSize, cy + iconSize), WithAlpha(120, 110, 86, opacity * pulse));
            float u = (float)(b.cell % kBuffIconCols * kBuffIconCell), v = (float)(b.cell / kBuffIconCols * kBuffIconCell);
            float in = floorf(2.0f * scale);
            draw->AddImage(ImTextureRef(sheets.items), ImVec2(cx + in, cy + in), ImVec2(cx + iconSize - in, cy + iconSize - in),
                ImVec2(u / sheets.itemsW, v / sheets.itemsH),
                ImVec2((u + kBuffIconCell) / sheets.itemsW, (v + kBuffIconCell) / sheets.itemsH),
                WithAlpha(255, 255, 255, opacity * pulse));
        } else if (IconRect(b.iconId, sheet, sx, sy, sw, sh) && sheet < 4 && sheets.tex[sheet]) {
            float tw = sheets.w[sheet], th = sheets.h[sheet];
            draw->AddImage(ImTextureRef(sheets.tex[sheet]), ImVec2(cx, cy), ImVec2(cx + iconSize, cy + iconSize),
                ImVec2(sx / tw, sy / th), ImVec2((sx + sw) / tw, (sy + sh) / th),
                WithAlpha(255, 255, 255, opacity * pulse));
        } else {
            draw->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + iconSize, cy + iconSize), WithAlpha(40, 36, 30, opacity));
        }

        int r, g, bl;
        TimeColor(b.remaining, r, g, bl);

        // drain bar: dark band, thin gold-grey trim, fill in the time colour
        float by = cy + iconSize + floorf(3.0f * scale);
        draw->AddRectFilled(ImVec2(cx - 1, by - 1), ImVec2(cx + iconSize + 1, by + barH + 1), WithAlpha(92, 84, 70, opacity * 0.9f));
        draw->AddRectFilled(ImVec2(cx, by), ImVec2(cx + iconSize, by + barH), WithAlpha(14, 14, 12, opacity));
        if (frac > 0.0f)
            draw->AddRectFilled(ImVec2(cx, by), ImVec2(cx + iconSize * frac, by + barH), WithAlpha(r, g, bl, opacity));

        char text[16];
        FormatTime(b.remaining, text, sizeof(text));
        ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, text);
        float tx = floorf(cx + (iconSize - ts.x) * 0.5f);
        float ty = by + barH + floorf(2.0f * scale);
        ImU32 shadow = WithAlpha(0, 0, 0, opacity * 0.9f);
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) draw->AddText(font, textSize, ImVec2(tx + dx, ty + dy), shadow, text);
        draw->AddText(font, textSize, ImVec2(tx, ty), WithAlpha(r, g, bl, opacity * pulse), text);
    }
}

} // namespace BuffTimers
