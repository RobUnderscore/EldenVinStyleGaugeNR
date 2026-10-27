#pragma once

struct GaugeConfig {
    // Position as fraction of screen (0.0 = left/top, 1.0 = right/bottom)
    float posX = 0.5f;        // horizontal center
    float posY = 0.88f;       // near bottom

    // Size
    float gaugeWidth = 300.0f;
    float gaugeHeight = 14.0f;
    float fontSize = 2.5f;

    // Transparency (0.0 = invisible, 1.0 = fully opaque)
    float opacity = 0.85f;

    // Show/hide elements
    bool showBar = true;
    bool showLabels = true;
    bool showBorder = false;

    // Buff timers (Elden Ring): icons of timed buffs with the time left under each
    bool buffTimers = true;
    float buffPosX = 0.052f;      // top-left corner of the row, as a fraction of the screen
    float buffPosY = 0.135f;
    float buffScale = 1.0f;       // 1.0 = 34px icons at 1080p, scales with resolution
    float buffPerRow = 10.0f;
    float buffMinDuration = 3.0f; // hide effects shorter than this many seconds in total
    float buffOpacity = 0.95f;
};

namespace Config {

GaugeConfig Load();
void Save(const GaugeConfig& cfg);

} // namespace Config
