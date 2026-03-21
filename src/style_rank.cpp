#include "style_rank.h"
#include "memory.h"
#include <Windows.h>

namespace StyleRank {

static constexpr int32_t STYLE_EFFECT_MIN = 999320410;
static constexpr int32_t STYLE_EFFECT_MAX = 999320429;
static constexpr int     MAX_RANK = 8;
static constexpr float   DEBOUNCE_TIME = 0.25f; // seconds

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

    uintptr_t node = head;
    int count = 0;
    int styleCount = 0;

    // Walk linked list, count active style effects
    do {
        int32_t id = Memory::Read<int32_t>(node + 0x08);

        if (id >= STYLE_EFFECT_MIN && id <= STYLE_EFFECT_MAX) {
            styleCount++;
        }

        count++;
        uintptr_t next = Memory::Read<uintptr_t>(node + 0x38);
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

    return state;
}

} // namespace StyleRank
