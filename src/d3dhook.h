#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstddef>
#include <cstdint>

struct ImFont;

namespace D3DHook {

bool Init();
void Shutdown();

// Called from hooked Present - set this to your render callback
using RenderCallback = void(*)(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue);
void SetRenderCallback(RenderCallback cb);

// F7 position mode
bool IsPositionMode();

// F8 convenience menu (owns mouse and keyboard while open, like F7)
bool IsMenuOpen();

// F6 hotkey list (display only, doesn't take input)
bool IsHotkeySheetOpen();

// Upload a block-compressed DDS (top mip only) into the overlay's descriptor heap (up to 10 textures in all).
// Returns its ImGui texture id, or 0 on failure. Call from the render callback.
uint64_t CreateTextureFromDds(const uint8_t* dds, size_t size);

// A texture filled region by region (the minimap's map tiles, Find an item's icons): its ImGui texture id, or 0.
// Block-compressed or DXGI_FORMAT_R8G8B8A8_UNORM. Starts transparent. Call from the render callback.
uint64_t CreateAtlas(UINT width, UINT height, UINT dxgiFormat);
// Copy a DDS's top mip (same format; block-compressed: position and size multiples of 4) into the atlas at x, y
bool UpdateAtlas(uint64_t atlas, UINT x, UINT y, const uint8_t* dds, size_t size);
// Several at once, waiting for the GPU once: how many were copied
struct AtlasRegion {
    UINT x, y;
    const uint8_t* dds;
    size_t size;
};
int UpdateAtlasMany(uint64_t atlas, const AtlasRegion* regions, int count);

// Serif font for the buff timers (nullptr if none could be loaded: use the default)
ImFont* TimerFont();

} // namespace D3DHook
