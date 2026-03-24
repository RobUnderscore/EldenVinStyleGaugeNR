#include "config.h"
#include <Windows.h>
#include <ShlObj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace Config {

static char g_DllConfigPath[MAX_PATH] = {};
static char g_UserConfigPath[MAX_PATH] = {};

static const char* GetDllConfigPath() {
    if (g_DllConfigPath[0] == 0) {
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&GetDllConfigPath, &hSelf);
        GetModuleFileNameA(hSelf, g_DllConfigPath, MAX_PATH);
        char* dot = strrchr(g_DllConfigPath, '.');
        if (dot && (dot - g_DllConfigPath + 4) < MAX_PATH)
            memcpy(dot, ".ini", 5); // includes null terminator
        else
            g_DllConfigPath[0] = 0; // can't form valid path
    }
    return g_DllConfigPath;
}

static const char* GetUserConfigPath() {
    if (g_UserConfigPath[0] == 0) {
        char appdata[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            const char* env = getenv("APPDATA");
            if (env) strncpy(appdata, env, MAX_PATH - 1);
            else return GetDllConfigPath(); // fallback
        }
        char dir[MAX_PATH];
        snprintf(dir, MAX_PATH, "%s\\VinStyleRankGauge", appdata);
        CreateDirectoryA(dir, nullptr);
        snprintf(g_UserConfigPath, MAX_PATH, "%s\\VinStyleRankGauge.ini", dir);
    }
    return g_UserConfigPath;
}

static float ReadFloat(FILE* f, const char* key, float defaultVal) {
    rewind(f);
    char line[256];
    size_t keyLen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        // Match "key = value" (key must be followed by '=' or whitespace)
        if (strncmp(line, key, keyLen) == 0 &&
            (line[keyLen] == '=' || line[keyLen] == ' ' || line[keyLen] == '\t')) {
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
        if (strncmp(line, key, keyLen) == 0 &&
            (line[keyLen] == '=' || line[keyLen] == ' ' || line[keyLen] == '\t')) {
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

static GaugeConfig LoadFrom(const char* path) {
    GaugeConfig cfg;
    FILE* f = fopen(path, "r");
    if (!f) return cfg;

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
    return cfg;
}

GaugeConfig Load() {
    // Try user config first (%APPDATA%), then DLL-relative
    const char* userPath = GetUserConfigPath();
    FILE* f = fopen(userPath, "r");
    if (f) {
        fclose(f);
        GaugeConfig cfg = LoadFrom(userPath);
        Log("Config: pos=(%.2f, %.2f) size=(%.0f, %.0f) font=%.1f opacity=%.2f bar=%d labels=%d border=%d",
            cfg.posX, cfg.posY, cfg.gaugeWidth, cfg.gaugeHeight, cfg.fontSize, cfg.opacity,
            cfg.showBar, cfg.showLabels, cfg.showBorder);
        return cfg;
    }

    const char* dllPath = GetDllConfigPath();
    f = fopen(dllPath, "r");
    if (f) {
        fclose(f);
        GaugeConfig cfg = LoadFrom(dllPath);
        Log("Config: pos=(%.2f, %.2f) size=(%.0f, %.0f) font=%.1f opacity=%.2f bar=%d labels=%d border=%d",
            cfg.posX, cfg.posY, cfg.gaugeWidth, cfg.gaugeHeight, cfg.fontSize, cfg.opacity,
            cfg.showBar, cfg.showLabels, cfg.showBorder);
        return cfg;
    }

    Log("Config: no config file found, writing defaults to %s", dllPath);
    WriteDefaultConfig(dllPath);
    return GaugeConfig{};
}

void Save(const GaugeConfig& cfg) {
    const char* path = GetUserConfigPath();
    FILE* f = fopen(path, "w");
    if (!f) {
        Log("Config: failed to save to %s", path);
        return;
    }

    fprintf(f, "# VinStyleRankGauge Configuration (saved by F7 position mode)\n");
    fprintf(f, "posX = %.4f\n", cfg.posX);
    fprintf(f, "posY = %.4f\n", cfg.posY);
    fprintf(f, "gaugeWidth = %.0f\n", cfg.gaugeWidth);
    fprintf(f, "gaugeHeight = %.0f\n", cfg.gaugeHeight);
    fprintf(f, "fontSize = %.1f\n", cfg.fontSize);
    fprintf(f, "opacity = %.2f\n", cfg.opacity);
    fprintf(f, "showBar = %d\n", cfg.showBar ? 1 : 0);
    fprintf(f, "showLabels = %d\n", cfg.showLabels ? 1 : 0);
    fprintf(f, "showBorder = %d\n", cfg.showBorder ? 1 : 0);
    fclose(f);

    Log("Config: saved position (%.4f, %.4f) to %s", cfg.posX, cfg.posY, path);
}

} // namespace Config
