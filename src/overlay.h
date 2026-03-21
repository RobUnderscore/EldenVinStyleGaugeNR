#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

namespace Overlay {

void Render(IDXGISwapChain3* swapChain, ID3D12CommandQueue* cmdQueue);

} // namespace Overlay
