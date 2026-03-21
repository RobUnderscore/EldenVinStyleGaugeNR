#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include "d3dhook.h"
#include "memory.h"
#include "overlay.h"

#pragma comment(lib, "psapi.lib")

static HANDLE g_InitThread = nullptr;
static FILE* g_LogFile = nullptr;

void Log(const char* fmt, ...) {
    if (!g_LogFile) {
        g_LogFile = fopen("VinStyleRankGauge.log", "a");
        if (!g_LogFile) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_LogFile, fmt, args);
    va_end(args);
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
}

static DWORD WINAPI InitThread(LPVOID hModule) {
    Log("VinStyleRankGauge initializing...");

    // Wait for the game to finish loading
    int waitCount = 0;
    while (!GetModuleHandleA("nightreign.exe")) {
        Sleep(1000);
        waitCount++;
        if (waitCount > 60) {
            Log("Timeout waiting for nightreign.exe");
            return 1;
        }
    }
    Log("nightreign.exe found, waiting for game init...");
    Sleep(5000);

    // Initialize memory reading
    Log("Initializing memory reader (AOB scan)...");
    bool memOk = false;
    for (int i = 0; i < 10; i++) {
        if (Memory::Init()) {
            memOk = true;
            break;
        }
        Log("AOB scan attempt %d failed, retrying...", i + 1);
        Sleep(2000);
    }
    if (memOk) {
        Log("Memory reader initialized, WorldChrMan at 0x%llX", Memory::GetWorldChrMan());
    } else {
        Log("WARNING: Memory reader failed, overlay will show but rank detection won't work");
    }

    // Initialize D3D12 hook
    Log("Initializing D3D12 hook...");
    if (D3DHook::Init()) {
        Log("D3D12 hook installed successfully");
        D3DHook::SetRenderCallback(Overlay::Render);
        Log("Overlay render callback set");
    } else {
        Log("ERROR: D3D12 hook failed!");
    }

    Log("Initialization complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_InitThread = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        Log("DLL unloading...");
        D3DHook::Shutdown();
        if (g_InitThread) {
            WaitForSingleObject(g_InitThread, 2000);
            CloseHandle(g_InitThread);
        }
        if (g_LogFile) fclose(g_LogFile);
        break;
    }
    return TRUE;
}
