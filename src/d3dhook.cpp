#include "d3dhook.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <Psapi.h>
#include <vector>
#include <cstdio>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

extern void Log(const char* fmt, ...);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace D3DHook {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain3*, UINT, UINT);
using ExecuteCommandListsFn = void(WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static ID3D12Device*                g_Device = nullptr;
static ID3D12DescriptorHeap*        g_SrvDescHeap = nullptr;
static ID3D12DescriptorHeap*        g_RtvDescHeap = nullptr;
static ID3D12CommandQueue*          g_CommandQueue = nullptr;
static ID3D12GraphicsCommandList*   g_CommandList = nullptr;
static std::vector<ID3D12CommandAllocator*> g_CommandAllocators;
static std::vector<ID3D12Resource*> g_RenderTargets;
static UINT                         g_FrameCount = 0;
static HWND                         g_GameWindow = nullptr;
static WNDPROC                      g_OriginalWndProc = nullptr;
static bool                         g_Initialized = false;
static bool                         g_InitAttempted = false;
static RenderCallback               g_RenderCallback = nullptr;
static PresentFn                    g_OrigPresent = nullptr;
static ExecuteCommandListsFn        g_OrigExecuteCommandLists = nullptr;

static void InitImGui(IDXGISwapChain3* swapChain);
static void CleanupRenderTargets();

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    return CallWindowProcW(g_OriginalWndProc, hWnd, msg, wParam, lParam);
}

static HRESULT WINAPI hkPresent(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!g_Initialized && !g_InitAttempted && g_CommandQueue) {
        g_InitAttempted = true;
        InitImGui(pSwapChain);
    }

    if (g_Initialized && g_CommandQueue) {
        __try {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (g_RenderCallback) {
                g_RenderCallback(pSwapChain, g_CommandQueue);
            }

            ImGui::EndFrame();

            UINT frameIndex = pSwapChain->GetCurrentBackBufferIndex();
            if (frameIndex >= g_FrameCount) {
                return g_OrigPresent(pSwapChain, SyncInterval, Flags);
            }

            auto& allocator = g_CommandAllocators[frameIndex];
            allocator->Reset();
            g_CommandList->Reset(allocator, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_RenderTargets[frameIndex];
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_CommandList->ResourceBarrier(1, &barrier);

            auto rtvHandle = g_RtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            rtvHandle.ptr += frameIndex * g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            g_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
            g_CommandList->SetDescriptorHeaps(1, &g_SrvDescHeap);

            ImGui::Render();
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_CommandList);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_CommandList->ResourceBarrier(1, &barrier);
            g_CommandList->Close();

            ID3D12CommandList* cmdLists[] = { g_CommandList };
            g_CommandQueue->ExecuteCommandLists(1, cmdLists);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Log once and disable overlay on crash
            static bool logged = false;
            if (!logged) {
                Log("EXCEPTION in hkPresent render! Disabling overlay.");
                logged = true;
            }
            g_Initialized = false;
        }
    }

    return g_OrigPresent(pSwapChain, SyncInterval, Flags);
}

static void WINAPI hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCmdLists, ID3D12CommandList* const* cmdLists) {
    if (!g_CommandQueue) {
        g_CommandQueue = queue;
    }
    g_OrigExecuteCommandLists(queue, numCmdLists, cmdLists);
}

static void InitImGui(IDXGISwapChain3* swapChain) {
    Log("InitImGui: starting...");

    DXGI_SWAP_CHAIN_DESC desc{};
    HRESULT hr = swapChain->GetDesc(&desc);
    if (FAILED(hr)) { Log("InitImGui: GetDesc failed 0x%08X", hr); return; }

    g_GameWindow = desc.OutputWindow;
    g_FrameCount = desc.BufferCount;
    Log("InitImGui: window=%p, bufferCount=%u, format=%u", g_GameWindow, g_FrameCount, desc.BufferDesc.Format);

    hr = swapChain->GetDevice(IID_PPV_ARGS(&g_Device));
    if (FAILED(hr) || !g_Device) { Log("InitImGui: GetDevice failed 0x%08X", hr); return; }
    Log("InitImGui: device=%p", g_Device);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_SrvDescHeap));
    if (FAILED(hr)) { Log("InitImGui: SRV heap failed 0x%08X", hr); return; }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = g_FrameCount;
    hr = g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvDescHeap));
    if (FAILED(hr)) { Log("InitImGui: RTV heap failed 0x%08X", hr); return; }

    g_CommandAllocators.resize(g_FrameCount);
    g_RenderTargets.resize(g_FrameCount);

    auto rtvIncrement = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvHandle = g_RtvDescHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < g_FrameCount; i++) {
        hr = g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
        if (FAILED(hr)) { Log("InitImGui: CommandAllocator %u failed 0x%08X", i, hr); return; }
        hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        if (FAILED(hr)) { Log("InitImGui: GetBuffer %u failed 0x%08X", i, hr); return; }
        g_Device->CreateRenderTargetView(g_RenderTargets[i], nullptr, rtvHandle);
        rtvHandle.ptr += rtvIncrement;
    }

    hr = g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[0], nullptr, IID_PPV_ARGS(&g_CommandList));
    if (FAILED(hr)) { Log("InitImGui: CommandList failed 0x%08X", hr); return; }
    g_CommandList->Close();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_Init(g_GameWindow);

    // Use the new InitInfo API which includes CommandQueue for font upload
    DXGI_FORMAT renderFormat = desc.BufferDesc.Format;
    Log("InitImGui: using render format %u with new InitInfo API", renderFormat);

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = g_Device;
    initInfo.CommandQueue = g_CommandQueue;
    initInfo.NumFramesInFlight = static_cast<int>(g_FrameCount);
    initInfo.RTVFormat = renderFormat;
    initInfo.SrvDescriptorHeap = g_SrvDescHeap;
    initInfo.LegacySingleSrvCpuDescriptor = g_SrvDescHeap->GetCPUDescriptorHandleForHeapStart();
    initInfo.LegacySingleSrvGpuDescriptor = g_SrvDescHeap->GetGPUDescriptorHandleForHeapStart();

    if (!ImGui_ImplDX12_Init(&initInfo)) {
        Log("InitImGui: ImGui_ImplDX12_Init failed!");
        return;
    }
    Log("InitImGui: ImGui DX12 initialized");

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    g_Initialized = true;
    Log("InitImGui: success!");
}

void SetRenderCallback(RenderCallback cb) {
    g_RenderCallback = cb;
}

bool Init() {
    Log("D3DHook::Init starting vtable discovery...");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"VinStyleRankTemp";
    if (!RegisterClassExW(&wc)) {
        Log("RegisterClassEx failed: %lu", GetLastError());
        return false;
    }

    HWND tempWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    if (!tempWnd) {
        Log("CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    Log("Temp window created");

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        Log("CreateDXGIFactory1 failed: 0x%08X", hr);
        DestroyWindow(tempWnd);
        return false;
    }
    Log("DXGI factory created");

    ID3D12Device* tempDevice = nullptr;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tempDevice));
    if (FAILED(hr) || !tempDevice) {
        Log("D3D12CreateDevice failed: 0x%08X", hr);
        factory->Release();
        DestroyWindow(tempWnd);
        return false;
    }
    Log("D3D12 temp device created");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tempQueue = nullptr;
    hr = tempDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tempQueue));
    if (FAILED(hr) || !tempQueue) {
        Log("CreateCommandQueue failed: 0x%08X", hr);
        tempDevice->Release();
        factory->Release();
        DestroyWindow(tempWnd);
        return false;
    }
    Log("Temp command queue created");

    DXGI_SWAP_CHAIN_DESC scDesc{};
    scDesc.BufferCount = 2;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = tempWnd;
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* tempSwapChain = nullptr;
    hr = factory->CreateSwapChain(tempQueue, &scDesc, &tempSwapChain);
    if (FAILED(hr) || !tempSwapChain) {
        Log("CreateSwapChain failed: 0x%08X (this may be expected under Proton)", hr);
        // Try alternative: DXGI_SWAP_EFFECT_DISCARD
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(tempQueue, &scDesc, &tempSwapChain);
        if (FAILED(hr) || !tempSwapChain) {
            Log("CreateSwapChain (fallback) also failed: 0x%08X", hr);
            tempQueue->Release();
            tempDevice->Release();
            factory->Release();
            DestroyWindow(tempWnd);
            return false;
        }
        Log("Swap chain created with DISCARD fallback");
    }
    Log("Temp swap chain created");

    void** swapChainVtable = *reinterpret_cast<void***>(tempSwapChain);
    void** queueVtable = *reinterpret_cast<void***>(tempQueue);

    auto presentAddr = swapChainVtable[8];   // IDXGISwapChain::Present
    auto execCmdAddr = queueVtable[10];      // ID3D12CommandQueue::ExecuteCommandLists
    Log("VTable Present=%p, ExecuteCommandLists=%p", presentAddr, execCmdAddr);

    tempSwapChain->Release();
    tempQueue->Release();
    tempDevice->Release();
    factory->Release();
    DestroyWindow(tempWnd);
    UnregisterClassW(wc.lpszClassName, nullptr);
    Log("Temp objects cleaned up");

    MH_STATUS mhStatus = MH_Initialize();
    Log("MH_Initialize: %d", mhStatus);

    mhStatus = MH_CreateHook(presentAddr, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&g_OrigPresent));
    Log("MH_CreateHook(Present): %d", mhStatus);

    mhStatus = MH_CreateHook(execCmdAddr, reinterpret_cast<void*>(&hkExecuteCommandLists), reinterpret_cast<void**>(&g_OrigExecuteCommandLists));
    Log("MH_CreateHook(ExecuteCommandLists): %d", mhStatus);

    mhStatus = MH_EnableHook(MH_ALL_HOOKS);
    Log("MH_EnableHook: %d", mhStatus);

    return true;
}

void Shutdown() {
    Log("D3DHook::Shutdown");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_OriginalWndProc && g_GameWindow) {
        SetWindowLongPtrW(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
    }

    if (g_Initialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    CleanupRenderTargets();
}

static void CleanupRenderTargets() {
    for (auto& rt : g_RenderTargets) { if (rt) { rt->Release(); rt = nullptr; } }
    for (auto& alloc : g_CommandAllocators) { if (alloc) { alloc->Release(); alloc = nullptr; } }
    if (g_CommandList) { g_CommandList->Release(); g_CommandList = nullptr; }
    if (g_SrvDescHeap) { g_SrvDescHeap->Release(); g_SrvDescHeap = nullptr; }
    if (g_RtvDescHeap) { g_RtvDescHeap->Release(); g_RtvDescHeap = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
}

} // namespace D3DHook
