#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstddef>
#include <cstdint>

namespace D3DHook {

bool Init();
void Shutdown();

// Called from hooked Present - set this to your render callback
using RenderCallback = void(*)(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue);
void SetRenderCallback(RenderCallback cb);

// F7 position mode
bool IsPositionMode();

// Upload a block-compressed DDS (top mip only) into the overlay's descriptor heap.
// Returns its ImGui texture id, or 0 on failure. Call from the render callback.
uint64_t CreateTextureFromDds(const uint8_t* dds, size_t size);

} // namespace D3DHook
