#include "style_rank.h"
#include "memory.h"
#include <Windows.h>

namespace StyleRank {

static constexpr int32_t STYLE_EFFECT_MIN = 999320410;
static constexpr int32_t STYLE_EFFECT_MAX = 999320429;
static constexpr int     MAX_RANK = 8;
static constexpr float   DEBOUNCE_TIME = 0.25f; // seconds

// Elden Vins (ER) Ultimate Arts: the accumulator entry points at its counter state.
// Hits add points, the count drains 1 per motionInterval, and the entry fires at its
// upper trigger count (450). Offsets found live on ER 2.7.2.0.
static constexpr ptrdiff_t ACC_STATE = 0x10;     // node -> accumulator state
static constexpr ptrdiff_t ACC_COUNT = 0x08;     // state -> current count (int)
static constexpr ptrdiff_t ACC_THRESHOLD = 0x18; // node -> upper trigger count (int)

static int   s_displayedRank = 0;
static int   s_candidateRank = 0;
static LARGE_INTEGER s_candidateSince = {};
static LARGE_INTEGER s_perfFreq = {};
static bool  s_perfInit = false;

static float GetTime() {
    if (!s_perfInit) {
        QueryPerformanceFrequency(&s_perfFreq);
        s_perfInit = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (float)now.QuadPart / (float)s_perfFreq.QuadPart;
}

StyleRankState Update() {
    StyleRankState state{};

    uintptr_t head = Memory::GetSpEffectListHead();
    if (head == 0) return state;
    const Memory::GameLayout* game = Memory::Game();

    uintptr_t node = head;
    int count = 0;
    int styleCount = 0;

    float minDur = 999.0f;
    float maxDur = 0.0f;
    uintptr_t ultNode = 0;

    // Walk linked list, count active style effects
    do {
        int32_t id = Memory::Read<int32_t>(node + 0x08);

        if (id >= STYLE_EFFECT_MIN && id <= STYLE_EFFECT_MAX) {
            styleCount++;
            float dur = Memory::Read<float>(node + game->nodeRemaining);
            if (dur > 0.0f) {
                if (dur < minDur) minDur = dur;
                if (dur > maxDur) maxDur = dur;
            }
        }

        if (game->ultEffectId != 0 && id == game->ultEffectId) ultNode = node;

        count++;
        uintptr_t next = Memory::Read<uintptr_t>(node + game->nodeNext);
        if (next == 0 || next == head || next == node || count > 500) break;
        node = next;
    } while (true);

    int rawRank = (styleCount > MAX_RANK) ? MAX_RANK : styleCount;

    // Debounce: rank must hold for DEBOUNCE_TIME before display updates
    float now = GetTime();
    if (rawRank != s_candidateRank) {
        s_candidateRank = rawRank;
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        s_candidateSince = li;
    } else if (rawRank != s_displayedRank) {
        float elapsed = now - (float)s_candidateSince.QuadPart / (float)s_perfFreq.QuadPart;
        if (elapsed >= DEBOUNCE_TIME) {
            s_displayedRank = s_candidateRank;
        }
    }

    state.effectCount = styleCount;
    state.rank = s_displayedRank;
    state.minDuration = (styleCount > 0 && minDur < 999.0f) ? minDur : 0.0f;
    state.maxDuration = maxDur;

    // No style rank running: show the Ultimate charge instead, if this game has one
    if (styleCount == 0 && ultNode != 0) {
        int32_t threshold = Memory::Read<int32_t>(ultNode + ACC_THRESHOLD);
        uintptr_t acc = Memory::Read<uintptr_t>(ultNode + ACC_STATE);
        int32_t charge = acc ? Memory::Read<int32_t>(acc + ACC_COUNT) : 0;
        if (threshold > 0 && charge > 0) {
            float fill = (charge >= threshold) ? 1.0f : (float)charge / (float)threshold;
            state.fill = fill;
            state.rank = (fill >= 1.0f) ? MAX_RANK : 1 + (int)(fill * (MAX_RANK - 1));
            state.label = (fill >= 1.0f) ? "READY" : "ULT";
        }
    }

    return state;
}

} // namespace StyleRank
