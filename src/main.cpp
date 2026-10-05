#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>
#include <iostream>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "audio/graph_engine.h"
#include "audio/asio_manager.h"
#include "tools/tuner.h"
#include "tools/metronome.h"
#include "midi/midi_manager.h"
#include "state/scene_manager.h"
#include "plugins/builtin_dsp.h"
#include "ui/theme.h"
#include "ui/rack_view.h"

// Forward declare Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Enable High-DPI awareness
    SetProcessDPIAware();

    // Register Win32 window class
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"PraccyHostClass", nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Praccy - ASIO VST3/CLAP Practice Host",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    praccy::ui::applyPraccyTheme();

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ==========================================
    // Initialize Praccy Core Subsystems
    // ==========================================
    praccy::audio::GraphEngine graph;
    graph.prepare(48000.0, 256);

    // Populate default guitar practice rig
    auto drive = std::make_unique<praccy::plugins::OverdriveEffect>();
    auto amp = std::make_unique<praccy::plugins::TubeAmpEffect>();
    auto delay = std::make_unique<praccy::plugins::StereoDelayEffect>();

    graph.addSerialNode(std::make_unique<praccy::audio::PluginSlot>(std::move(drive)));
    graph.addSerialNode(std::make_unique<praccy::audio::PluginSlot>(std::move(amp)));
    graph.addSerialNode(std::make_unique<praccy::audio::PluginSlot>(std::move(delay)));

    praccy::tools::InstrumentTuner tuner;
    tuner.prepare(48000.0);

    praccy::tools::Metronome metronome;
    metronome.prepare(48000.0);

    praccy::midi::MidiManager midi;
    praccy::state::SceneManager scenes;

    // Capture initial rig into Scene 1
    scenes.captureCurrentScene(0, graph);

    // Initialize ASIO
    praccy::audio::AsioManager asio;
    asio.setAudioCallback([&](const praccy::audio::AudioBufferView& in, praccy::audio::AudioBufferView& out) {
        if (in.numChannels() > 0) {
            tuner.process(in.channel(0), in.numSamples());
        }
        graph.process(in, out);
        metronome.process(out);
    });

    // Auto-select and start available ASIO driver (prefer dedicated hardware USB interfaces)
    auto drivers = praccy::audio::AsioManager::enumerateDrivers();
    int activeDriverIdx = -1;
    for (size_t i = 0; i < drivers.size(); ++i) {
        if (drivers[i].name.find("USB") != std::string::npos) {
            if (asio.loadDriver(drivers[i], hwnd)) {
                activeDriverIdx = static_cast<int>(i);
                asio.start();
                break;
            }
        }
    }
    if (activeDriverIdx == -1) {
        for (size_t i = 0; i < drivers.size(); ++i) {
            if (asio.loadDriver(drivers[i], hwnd)) {
                activeDriverIdx = static_cast<int>(i);
                asio.start();
                break;
            }
        }
    }

    // Connect MIDI bindings callback
    midi.setBindingCallback([&](const praccy::midi::MidiBinding& binding, float normVal) {
        if (binding.targetType == praccy::midi::BindingTargetType::SlotBypass) {
            auto* node = graph.getNode(binding.targetSlotIndex);
            if (node) node->setBypassed(!node->isBypassed());
        } else if (binding.targetType == praccy::midi::BindingTargetType::SlotDryWet) {
            auto* slot = dynamic_cast<praccy::audio::PluginSlot*>(graph.getNode(binding.targetSlotIndex));
            if (slot) slot->setDryWet(normVal);
        } else if (binding.targetType == praccy::midi::BindingTargetType::SceneSelect) {
            scenes.applyScene(binding.targetSceneIndex, graph);
        } else if (binding.targetType == praccy::midi::BindingTargetType::MasterVolume) {
            graph.setMasterVolumeDb(-36.0f + (normVal * 42.0f));
        }
    });

    praccy::ui::RackView rackView(graph, asio, tuner, metronome, midi, scenes);

    // Main event loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        // Start Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        rackView.render();

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.11f, 0.12f, 0.14f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present with vsync (1) to keep CPU overhead at near-zero
        g_pSwapChain->Present(1, 0);
    }

    asio.stop();
    asio.unloadDriver();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                if (g_pd3dDevice != nullptr && g_pSwapChain != nullptr) {
                    CleanupRenderTarget();
                    g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                    CreateRenderTarget();
                }
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
