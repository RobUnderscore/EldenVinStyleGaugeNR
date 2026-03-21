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
};

namespace Config {

GaugeConfig Load();

} // namespace Config
