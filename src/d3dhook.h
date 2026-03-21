#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

namespace D3DHook {

bool Init();
void Shutdown();

// Called from hooked Present - set this to your render callback
using RenderCallback = void(*)(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue);
void SetRenderCallback(RenderCallback cb);

// F7 position mode
bool IsPositionMode();

} // namespace D3DHook
