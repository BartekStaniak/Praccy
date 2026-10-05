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
#include "plugins/plugin_scanner.h"
#include "plugins/plugin_window.h"
#include "state/app_config.h"
#include "ui/theme.h"
#include "ui/rack_view.h"
#include "ui/thumbnail_manager.h"

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

    // Load application icon from embedded resource
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!hIcon) {
        hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    }

    // Register Win32 window class with dark background brush matching Praccy theme
    HBRUSH hDarkBrush = CreateSolidBrush(RGB(28, 30, 36));
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        hInstance, hIcon, LoadCursor(nullptr, IDC_ARROW), hDarkBrush, nullptr,
        L"PraccyHostClass", hIcon
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Praccy - ASIO VST3/CLAP Practice Host",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (hIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (hDarkBrush) DeleteObject(hDarkBrush);
        return 1;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    praccy::ui::applyPraccyTheme();

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x2600, 0x26FF, // Miscellaneous Symbols (★, ☆, etc.)
        0,
    };
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig, glyphRanges);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    praccy::ui::ThumbnailManager::instance().init(g_pd3dDevice);

    // ==========================================
    // Initialize Praccy Core Subsystems & Config
    // ==========================================
    praccy::state::AppConfig appConfig;
    bool configLoaded = appConfig.load();

    praccy::audio::GraphEngine graph;
    graph.prepare(48000.0, 256);

    if (configLoaded) {
        praccy::audio::InputRoutingConfig inCfg;
        inCfg.mode = appConfig.inputMode;
        graph.setInputRouting(inCfg);
        graph.setInputGainDb(appConfig.inputGainDb);
        graph.setMasterVolumeDb(appConfig.masterVolumeDb);
    }

    // Default state: clean direct signal path from Input to Output
    praccy::tools::InstrumentTuner tuner;
    tuner.prepare(48000.0);

    praccy::tools::Metronome metronome;
    metronome.prepare(48000.0);
    if (configLoaded) {
        metronome.setBpm(appConfig.metronomeBpm);
    }

    praccy::midi::MidiManager midi;
    praccy::state::SceneManager scenes;

    // Initialize ASIO
    praccy::audio::AsioManager asio;
    asio.setAudioCallback([&](const praccy::audio::AudioBufferView& in, praccy::audio::AudioBufferView& out) {
        if (in.numChannels() > 0) {
            tuner.process(in.channel(0), in.numSamples());
        }
        graph.process(in, out);
        metronome.process(out);
    });

    // Auto-select and start ASIO driver (prioritizing last saved driver, then hardware USB)
    auto drivers = praccy::audio::AsioManager::enumerateDrivers();
    int activeDriverIdx = -1;

    // 1. Try to restore last saved driver
    if (configLoaded && !appConfig.lastAsioDriver.empty()) {
        for (size_t i = 0; i < drivers.size(); ++i) {
            if (drivers[i].name == appConfig.lastAsioDriver) {
                if (asio.loadDriver(drivers[i], hwnd)) {
                    activeDriverIdx = static_cast<int>(i);
                    asio.start();
                    break;
                }
            }
        }
    }

    // 2. Fall back to dedicated hardware USB interfaces
    if (activeDriverIdx == -1) {
        for (size_t i = 0; i < drivers.size(); ++i) {
            if (drivers[i].name.find("USB") != std::string::npos) {
                if (asio.loadDriver(drivers[i], hwnd)) {
                    activeDriverIdx = static_cast<int>(i);
                    asio.start();
                    break;
                }
            }
        }
    }

    // 3. Fall back to first working driver
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

    praccy::plugins::PluginScanner scanner;
    if (configLoaded) {
        for (const auto& cp : appConfig.customPluginPaths) {
            scanner.addCustomSearchPath(cp);
        }
    }
    // Launch asynchronous non-blocking plugin scanner in the background
    scanner.scanAll();

    praccy::ui::RackView rackView(graph, asio, tuner, metronome, midi, scenes, scanner);

    // Pre-render and present the first frame to the swapchain backbuffer
    // BEFORE showing the window. This completely eliminates any white window flash or hang,
    // ensuring the window appears instantly in its fully styled dark state.
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    rackView.render();
    ImGui::Render();
    const float clear_color[4] = { 0.11f, 0.12f, 0.14f, 1.00f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(0, 0);

    // Display window now that the first frame is already drawn and ready
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

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
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present with vsync (1) to keep CPU overhead at near-zero
        g_pSwapChain->Present(1, 0);
    }

    // Persist configuration
    appConfig.inputMode = graph.inputRouting().mode;
    appConfig.inputGainDb = graph.inputGainDb();
    appConfig.masterVolumeDb = graph.masterVolumeDb();
    appConfig.metronomeBpm = metronome.bpm();
    if (asio.isLoaded()) {
        appConfig.lastAsioDriver = asio.driverInfo().name;
    }
    appConfig.customPluginPaths = scanner.searchPaths();
    appConfig.save();

    // Close all open plugin GUI windows cleanly
    praccy::plugins::PluginWindowManager::instance().closeAllWindows();

    asio.stop();
    asio.unloadDriver();

    praccy::ui::ThumbnailManager::instance().shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (hDarkBrush) DeleteObject(hDarkBrush);

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
        case WM_ERASEBKGND:
            return 1; // Prevent GDI from erasing background with white (prevents flicker and white state)
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
