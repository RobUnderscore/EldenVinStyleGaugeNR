#include "d3dhook.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <Psapi.h>
#include <vector>
#include <cstdio>
#include <cstring>

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
static constexpr int                kMaxGameTextures = 7;
static ID3D12Resource*              g_GameTextures[kMaxGameTextures] = {}; // SRV slots 1.. (slot 0 is ImGui's font)
static int                          g_GameTextureCount = 0;
static ImFont*                      g_TimerFont = nullptr;
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
static bool                         g_PositionMode = false;
static bool                         g_MenuOpen = false;     // F8 convenience menu

// While dragging (F7) or in the menu (F8) the overlay owns the mouse and keyboard
static bool InputCaptured() { return g_PositionMode || g_MenuOpen; }

// Cursor hooks for position mode
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static ClipCursorFn       g_OrigClipCursor = nullptr;
static SetCursorPosFn     g_OrigSetCursorPos = nullptr;
static GetRawInputDataFn  g_OrigGetRawInputData = nullptr;
static RECT               g_SavedClipRect = {};
static bool               g_HadClipRect = false;

static BOOL WINAPI hkClipCursor(const RECT* lpRect) {
    if (InputCaptured()) {
        if (lpRect) { g_SavedClipRect = *lpRect; g_HadClipRect = true; }
        return TRUE;
    }
    return g_OrigClipCursor(lpRect);
}

static BOOL WINAPI hkSetCursorPos(int X, int Y) {
    if (InputCaptured()) return TRUE;
    return g_OrigSetCursorPos(X, Y);
}

static UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    if (InputCaptured() && pData != nullptr) {
        UINT result = g_OrigGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
        if (result != (UINT)-1 && uiCommand == RID_INPUT) {
            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                raw->data.mouse.lLastX = 0;
                raw->data.mouse.lLastY = 0;
                raw->data.mouse.ulButtons = 0;
                raw->data.mouse.usButtonData = 0;
            } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                raw->data.keyboard.VKey = 0;
                raw->data.keyboard.Message = 0;
            }
        }
        return result;
    }
    return g_OrigGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
}

static void InitImGui(IDXGISwapChain3* swapChain);
static void CleanupRenderTargets();

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // F7 toggles position mode, F8 the convenience menu (Esc also closes it)
    bool toggle = msg == WM_KEYDOWN && (wParam == VK_F7 || wParam == VK_F8 || (wParam == VK_ESCAPE && g_MenuOpen));
    if (toggle) {
        bool wasCaptured = InputCaptured();
        if (wParam == VK_F7) {
            g_PositionMode = !g_PositionMode;
            Log("Position mode: %s", g_PositionMode ? "ON (drag gauge)" : "OFF (locked)");
        } else {
            g_MenuOpen = wParam == VK_F8 ? !g_MenuOpen : false;
            Log("Convenience menu: %s", g_MenuOpen ? "open" : "closed");
        }

        if (InputCaptured() && !wasCaptured) {
            ImGui::GetIO().MouseDrawCursor = true;
            if (g_OrigClipCursor) g_OrigClipCursor(nullptr);
        } else if (!InputCaptured() && wasCaptured) {
            ImGui::GetIO().MouseDrawCursor = false;
            if (g_HadClipRect && g_OrigClipCursor) g_OrigClipCursor(&g_SavedClipRect);
        }
        return 0;
    }

    if (InputCaptured()) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;

        switch (msg) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_INPUT: case WM_SETCURSOR:
                return 0;
        }
    } else {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }

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
    // Capture ONLY the graphics queue. The game also submits on COMPUTE and COPY queues,
    // and the order it first touches them is not deterministic (especially under Proton).
    // Grabbing whichever queue submits first means we may later push our own DIRECT
    // command list onto a copy/compute queue, which removes the device -> black screen.
    if (!g_CommandQueue && queue) {
        if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_CommandQueue = queue;
            Log("Captured DIRECT command queue %p", (void*)queue);
        }
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
    srvHeapDesc.NumDescriptors = 1 + kMaxGameTextures;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_SrvDescHeap));
    if (FAILED(hr)) { Log("InitImGui: SRV heap failed 0x%08X", hr); CleanupRenderTargets(); return; }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = g_FrameCount;
    hr = g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvDescHeap));
    if (FAILED(hr)) { Log("InitImGui: RTV heap failed 0x%08X", hr); CleanupRenderTargets(); return; }

    g_CommandAllocators.resize(g_FrameCount);
    g_RenderTargets.resize(g_FrameCount);

    auto rtvIncrement = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvHandle = g_RtvDescHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < g_FrameCount; i++) {
        hr = g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
        if (FAILED(hr)) { Log("InitImGui: CommandAllocator %u failed 0x%08X", i, hr); CleanupRenderTargets(); return; }
        hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        if (FAILED(hr)) { Log("InitImGui: GetBuffer %u failed 0x%08X", i, hr); CleanupRenderTargets(); return; }
        g_Device->CreateRenderTargetView(g_RenderTargets[i], nullptr, rtvHandle);
        rtvHandle.ptr += rtvIncrement;
    }

    hr = g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[0], nullptr, IID_PPV_ARGS(&g_CommandList));
    if (FAILED(hr)) { Log("InitImGui: CommandList failed 0x%08X", hr); CleanupRenderTargets(); return; }
    g_CommandList->Close();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Default font stays first (the gauge uses it); a serif one for the buff timers' numbers.
    // times/georgia are in both Windows and Proton prefixes.
    io.Fonts->AddFontDefault();
    char fontDir[MAX_PATH] = {};
    GetWindowsDirectoryA(fontDir, MAX_PATH);
    for (const char* name : { "times.ttf", "georgia.ttf", "arial.ttf" }) {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\Fonts\\%s", fontDir, name);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if ((g_TimerFont = io.Fonts->AddFontFromFileTTF(path, 18.0f))) {
            Log("InitImGui: timer font %s", path);
            break;
        }
    }

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
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupRenderTargets();
        return;
    }
    Log("InitImGui: ImGui DX12 initialized");

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    g_Initialized = true;
    Log("InitImGui: success!");
}

uint64_t CreateTextureFromDds(const uint8_t* dds, size_t size) {
    if (!g_Initialized || g_GameTextureCount >= kMaxGameTextures || size < 0x94 ||
        memcmp(dds, "DDS ", 4) != 0 || memcmp(dds + 0x54, "DX10", 4) != 0) return 0;

    UINT height, width, dxgi;
    memcpy(&height, dds + 0x0C, 4);
    memcpy(&width, dds + 0x10, 4);
    memcpy(&dxgi, dds + 0x80, 4);
    DXGI_FORMAT format = (DXGI_FORMAT)dxgi;
    UINT blockBytes;
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        blockBytes = 8; break;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        blockBytes = 16; break;
    default:
        Log("CreateTextureFromDds: unsupported format %u", dxgi);
        return 0;
    }
    size_t rowBytes = (size_t)((width + 3) / 4) * blockBytes;
    UINT blockRows = (height + 3) / 4;
    if (0x94 + rowBytes * blockRows > size) { Log("CreateTextureFromDds: truncated DDS"); return 0; }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource* tex = nullptr;
    if (FAILED(g_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) {
        Log("CreateTextureFromDds: texture creation failed");
        return 0;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSize, uploadSize;
    g_Device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    if (SUCCEEDED(g_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && done) {
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            for (UINT r = 0; r < numRows; r++)
                memcpy(mapped + footprint.Offset + (size_t)r * footprint.Footprint.RowPitch,
                    dds + 0x94 + r * rowBytes, rowBytes);
            upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprint;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = tex;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
            list->Close();

            ID3D12CommandList* lists[] = { list };
            g_CommandQueue->ExecuteCommandLists(1, lists);
            g_CommandQueue->Signal(fence, 1);
            fence->SetEventOnCompletion(1, done);
            ok = WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
        }
    }
    if (done) CloseHandle(done);
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    if (!ok) { tex->Release(); Log("CreateTextureFromDds: upload failed"); return 0; }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    UINT inc = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_SrvDescHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_SrvDescHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += inc * (1 + g_GameTextureCount);
    gpu.ptr += inc * (1 + g_GameTextureCount);
    g_Device->CreateShaderResourceView(tex, &srv, cpu);
    g_GameTextures[g_GameTextureCount++] = tex;
    Log("CreateTextureFromDds: %ux%u format %u uploaded", width, height, dxgi);
    return gpu.ptr;
}

ImFont* TimerFont() {
    return g_TimerFont;
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

    // Cursor hooks for F7 position mode
    auto clipCursorAddr = GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor");
    auto setCursorPosAddr = GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursorPos");
    auto getRawInputDataAddr = GetProcAddress(GetModuleHandleA("user32.dll"), "GetRawInputData");

    if (clipCursorAddr) {
        mhStatus = MH_CreateHook(clipCursorAddr, reinterpret_cast<void*>(&hkClipCursor), reinterpret_cast<void**>(&g_OrigClipCursor));
        Log("MH_CreateHook(ClipCursor): %d", mhStatus);
    }
    if (setCursorPosAddr) {
        mhStatus = MH_CreateHook(setCursorPosAddr, reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&g_OrigSetCursorPos));
        Log("MH_CreateHook(SetCursorPos): %d", mhStatus);
    }
    if (getRawInputDataAddr) {
        mhStatus = MH_CreateHook(getRawInputDataAddr, reinterpret_cast<void*>(&hkGetRawInputData), reinterpret_cast<void**>(&g_OrigGetRawInputData));
        Log("MH_CreateHook(GetRawInputData): %d", mhStatus);
    }

    mhStatus = MH_EnableHook(MH_ALL_HOOKS);
    Log("MH_EnableHook: %d", mhStatus);

    return true;
}

bool IsMenuOpen() {
    return g_MenuOpen;
}

bool IsPositionMode() {
    return g_PositionMode;
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
    for (auto& t : g_GameTextures) if (t) { t->Release(); t = nullptr; }
    g_GameTextureCount = 0;
    if (g_SrvDescHeap) { g_SrvDescHeap->Release(); g_SrvDescHeap = nullptr; }
    if (g_RtvDescHeap) { g_RtvDescHeap->Release(); g_RtvDescHeap = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
}

} // namespace D3DHook
