#include "config.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace Config {

static char g_ConfigPath[MAX_PATH] = {};

static const char* GetConfigPath() {
    if (g_ConfigPath[0] == 0) {
        // Config file sits next to the DLL
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&GetConfigPath, &hSelf);
        GetModuleFileNameA(hSelf, g_ConfigPath, MAX_PATH);
        // Replace .dll with .ini
        char* dot = strrchr(g_ConfigPath, '.');
        if (dot) strcpy(dot, ".ini");
    }
    return g_ConfigPath;
}

static float ReadFloat(FILE* f, const char* key, float defaultVal) {
    rewind(f);
    char line[256];
    size_t keyLen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        // Match "key = value"
        if (strncmp(line, key, keyLen) == 0) {
            char* eq = strchr(line, '=');
            if (eq) return (float)atof(eq + 1);
        }
    }
    return defaultVal;
}

static bool ReadBool(FILE* f, const char* key, bool defaultVal) {
    rewind(f);
    char line[256];
    size_t keyLen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        if (strncmp(line, key, keyLen) == 0) {
            char* eq = strchr(line, '=');
            if (eq) {
                char* val = eq + 1;
                while (*val == ' ') val++;
                return (*val == '1' || *val == 't' || *val == 'T' || *val == 'y' || *val == 'Y');
            }
        }
    }
    return defaultVal;
}

static void WriteDefaultConfig(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# VinStyleRankGauge Configuration\n");
    fprintf(f, "#\n");
    fprintf(f, "# Position on screen (0.0 to 1.0)\n");
    fprintf(f, "#   posX: 0.0 = left, 0.5 = center, 1.0 = right\n");
    fprintf(f, "#   posY: 0.0 = top, 0.5 = middle, 1.0 = bottom\n");
    fprintf(f, "posX = 0.5\n");
    fprintf(f, "posY = 0.88\n");
    fprintf(f, "\n");
    fprintf(f, "# Size\n");
    fprintf(f, "gaugeWidth = 300\n");
    fprintf(f, "gaugeHeight = 14\n");
    fprintf(f, "fontSize = 2.5\n");
    fprintf(f, "\n");
    fprintf(f, "# Transparency (0.0 = invisible, 1.0 = fully opaque)\n");
    fprintf(f, "opacity = 0.85\n");
    fprintf(f, "\n");
    fprintf(f, "# Show/hide elements (1 = show, 0 = hide)\n");
    fprintf(f, "showBar = 1\n");
    fprintf(f, "showLabels = 1\n");
    fprintf(f, "showBorder = 0\n");
    fclose(f);
}

GaugeConfig Load() {
    GaugeConfig cfg;
    const char* path = GetConfigPath();

    FILE* f = fopen(path, "r");
    if (!f) {
        Log("Config: no config file found, writing defaults to %s", path);
        WriteDefaultConfig(path);
        return cfg;
    }

    Log("Config: loading from %s", path);
    cfg.posX = ReadFloat(f, "posX", cfg.posX);
    cfg.posY = ReadFloat(f, "posY", cfg.posY);
    cfg.gaugeWidth = ReadFloat(f, "gaugeWidth", cfg.gaugeWidth);
    cfg.gaugeHeight = ReadFloat(f, "gaugeHeight", cfg.gaugeHeight);
    cfg.fontSize = ReadFloat(f, "fontSize", cfg.fontSize);
    cfg.opacity = ReadFloat(f, "opacity", cfg.opacity);
    cfg.showBar = ReadBool(f, "showBar", cfg.showBar);
    cfg.showLabels = ReadBool(f, "showLabels", cfg.showLabels);
    cfg.showBorder = ReadBool(f, "showBorder", cfg.showBorder);
    fclose(f);

    Log("Config: pos=(%.2f, %.2f) size=(%.0f, %.0f) font=%.1f opacity=%.2f bar=%d labels=%d border=%d",
        cfg.posX, cfg.posY, cfg.gaugeWidth, cfg.gaugeHeight, cfg.fontSize, cfg.opacity,
        cfg.showBar, cfg.showLabels, cfg.showBorder);
    return cfg;
}

} // namespace Config
