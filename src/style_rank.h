#pragma once
#include <cstdint>
#include <string>

struct StyleRankState {
    int rank = 0;           // 0=None, 1=D, 2=C, 3=B, 4=A, 5=S, 6=SS, 7=SSS, 8=V
    int effectCount = 0;    // Number of active style effects
    float minDuration = 0;  // Shortest remaining duration (closest to expiring)
    float maxDuration = 0;  // Longest remaining duration (freshest hit)
    float maxPossible = 5.0f; // Max duration per effect (5 seconds)

    const char* GetRankName() const {
        static const char* names[] = {
            "", "D", "C", "B", "A", "S", "SS", "SSS", "V"
        };
        if (rank < 0 || rank > 8) return "?";
        return names[rank];
    }

    // Gauge fill percentage (0.0 to 1.0) based on proximity to next rank
    // Uses min duration as the "gauge within current rank"
    float GetGaugeFill() const {
        if (effectCount == 0) return 0.0f;
        return minDuration / maxPossible;
    }
};

namespace StyleRank {

// Update the style rank state by walking the SpEffect linked list
StyleRankState Update();

} // namespace StyleRank
