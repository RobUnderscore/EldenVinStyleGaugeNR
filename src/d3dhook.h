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

// Upload a block-compressed DDS (top mip only) into the overlay's descriptor heap (up to 7 of them).
// Returns its ImGui texture id, or 0 on failure. Call from the render callback.
uint64_t CreateTextureFromDds(const uint8_t* dds, size_t size);

// Serif font for the buff timers (nullptr if none could be loaded: use the default)
ImFont* TimerFont();

} // namespace D3DHook
