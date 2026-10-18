#include "plugin_window.h"
#include <dwmapi.h>
#include <iostream>
#include <chrono>
#include <thread>

namespace praccy::plugins {

PluginWindowManager& PluginWindowManager::instance() {
    static PluginWindowManager s_instance;
    return s_instance;
}

PluginWindowManager::PluginWindowManager() {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    if (!hIcon) hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    m_bgCol = RGB(18, 19, 22);
    m_bgBrush = CreateSolidBrush(m_bgCol);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PluginWindowManager::windowProc;
    wc.hInstance = hInst;
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_bgBrush;
    wc.lpszClassName = L"PraccyPluginHostClass";

    m_windowClassAtom = RegisterClassExW(&wc);
}

PluginWindowManager::~PluginWindowManager() {
    closeAllWindows();
    if (m_windowClassAtom) {
        UnregisterClassW(reinterpret_cast<LPCWSTR>(m_windowClassAtom), GetModuleHandle(nullptr));
    }
    if (m_bgBrush) {
        DeleteObject(m_bgBrush);
        m_bgBrush = nullptr;
    }
}

bool PluginWindowManager::isWindowOpening(IPluginInstance* plugin) const {
    if (!plugin) return false;
    return m_openingPlugins.find(plugin) != m_openingPlugins.end();
}

bool PluginWindowManager::openPluginWindow(IPluginInstance* plugin) {
    if (!plugin) return false;
    if (isWindowOpen(plugin)) {
        return openPluginWindowAsync(plugin, nullptr);
    }
    bool finished = false;
    bool result = false;
    if (!openPluginWindowAsync(plugin, [&](bool success) {
        finished = true;
        result = success;
    })) {
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    while (!finished) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        update();
        if (finished) break;
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() > 35) {
            std::cerr << "Timeout waiting for openPluginWindow: " << plugin->name() << "\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return result;
}

bool PluginWindowManager::openPluginWindowAsync(IPluginInstance* plugin, std::function<void(bool success)> onComplete) {
    if (!plugin) return false;

    // If window already open, bring to front, restore without forcing global HWND_TOPMOST
    auto it = m_openWindows.find(plugin);
    if (it != m_openWindows.end() && IsWindow(it->second)) {
        HWND existingHwnd = it->second;
        ShowWindow(existingHwnd, SW_RESTORE);
        SetWindowPos(existingHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(existingHwnd);
        SetForegroundWindow(existingHwnd);
        if (onComplete) onComplete(true);
        return true;
    }

    // If already opening in background, avoid duplicate requests
    if (isWindowOpening(plugin)) {
        return true;
    }

    // Determine initial preferred size from plugin
    int prefW = 850;
    int prefH = 600;
    plugin->getPreferredSize(prefW, prefH);

    std::wstring wTitle;
    const std::string& pName = plugin->name();
    if (!pName.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pName.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            wTitle.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, pName.c_str(), -1, wTitle.data(), wlen);
        }
    }
    wTitle += L" - Praccy";

    // Calculate window size including borders for desired client area
    DWORD style = WS_OVERLAPPEDWINDOW;
    // Use m_mainHwnd as owner window instead of global WS_EX_TOPMOST so plugin windows don't cover external apps on Alt-Tab
    DWORD exStyle = m_mainHwnd ? WS_EX_APPWINDOW : (WS_EX_APPWINDOW | WS_EX_TOPMOST);
    RECT wr = {0, 0, prefW, prefH};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    // Always center the window on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = std::max(0, (screenW - winW) / 2);
    int posY = std::max(0, (screenH - winH) / 2);

    // Create window initially hidden so no empty frame flickers while attaching GUI
    HWND hwnd = CreateWindowExW(
        exStyle,
        reinterpret_cast<LPCWSTR>(m_windowClassAtom),
        wTitle.c_str(),
        style,
        posX, posY,
        winW, winH,
        m_mainHwnd, nullptr,
        GetModuleHandle(nullptr),
        this
    );

    if (hwnd) {
        BOOL useDark = m_isDark ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
            DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
        }
        SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(m_bgBrush));
    }

    if (!hwnd) {
        std::cerr << "Failed to create Win32 window for plugin: " << plugin->name() << "\n";
        if (onComplete) onComplete(false);
        return false;
    }

    SetPropW(hwnd, L"PluginInstance", reinterpret_cast<HANDLE>(plugin));

    m_openingPlugins.insert(plugin);
    m_pendingWindows[plugin] = hwnd;

    plugin->openGuiAsync(hwnd, [this, plugin, hwnd, onComplete = std::move(onComplete)](bool success) {
        if (m_openingPlugins.find(plugin) == m_openingPlugins.end() || !IsWindow(hwnd)) {
            m_openingPlugins.erase(plugin);
            m_pendingWindows.erase(plugin);
            if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
            if (onComplete) onComplete(false);
            return;
        }
        m_openingPlugins.erase(plugin);
        m_pendingWindows.erase(plugin);

        if (!success) {
            std::cerr << "plugin->openGuiAsync returned false for: " << plugin->name() << "\n";
            DestroyWindow(hwnd);
            if (onComplete) onComplete(false);
            return;
        }

        // Check if plugin size changed after attaching GUI
        int actualW = 850;
        int actualH = 600;
        plugin->getPreferredSize(actualW, actualH);
        DWORD actStyle = WS_OVERLAPPEDWINDOW;
        DWORD actExStyle = m_mainHwnd ? WS_EX_APPWINDOW : (WS_EX_APPWINDOW | WS_EX_TOPMOST);
        RECT actualWr = {0, 0, actualW, actualH};
        AdjustWindowRectEx(&actualWr, actStyle, FALSE, actExStyle);
        int actWinW = actualWr.right - actualWr.left;
        int actWinH = actualWr.bottom - actualWr.top;
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        int actPosX = std::max(0, (scrW - actWinW) / 2);
        int actPosY = std::max(0, (scrH - actWinH) / 2);

        m_openWindows[plugin] = hwnd;
        m_windowToPlugin[hwnd] = plugin;

        // Position centered and at top of owner z-order without locking HWND_TOPMOST over external apps
        SetWindowPos(hwnd, HWND_TOP, actPosX, actPosY, actWinW, actWinH, SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);

        if (onComplete) onComplete(true);
    });

    return true;
}

void PluginWindowManager::update() {
    if (m_openingPlugins.empty()) return;
    std::vector<IPluginInstance*> openingList(m_openingPlugins.begin(), m_openingPlugins.end());
    for (auto* p : openingList) {
        if (p) {
            p->pollAsyncGuiResults();
        }
    }
}

void PluginWindowManager::closePluginWindow(IPluginInstance* plugin) {
    auto pendIt = m_pendingWindows.find(plugin);
    if (pendIt != m_pendingWindows.end()) {
        HWND hwnd = pendIt->second;
        m_pendingWindows.erase(pendIt);
        m_openingPlugins.erase(plugin);
        plugin->closeGui();
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    auto it = m_openWindows.find(plugin);
    if (it != m_openWindows.end()) {
        HWND hwnd = it->second;
        m_openWindows.erase(it);
        m_windowToPlugin.erase(hwnd);
        plugin->closeGui();
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
}

void PluginWindowManager::closeAllWindows() {
    for (auto& [plugin, hwnd] : m_pendingWindows) {
        if (plugin) plugin->closeGui();
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    m_pendingWindows.clear();
    m_openingPlugins.clear();

    for (auto& [plugin, hwnd] : m_openWindows) {
        if (plugin) plugin->closeGui();
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    m_openWindows.clear();
    m_windowToPlugin.clear();
}

bool PluginWindowManager::isWindowOpen(IPluginInstance* plugin) const {
    if (!plugin) return false;
    auto it = m_openWindows.find(plugin);
    return (it != m_openWindows.end() && IsWindow(it->second));
}

HWND PluginWindowManager::getWindow(IPluginInstance* plugin) const {
    if (!plugin) return nullptr;
    auto it = m_openWindows.find(plugin);
    return (it != m_openWindows.end()) ? it->second : nullptr;
}

void PluginWindowManager::updateTitleBars(bool isDark) {
    updateTheme(isDark, m_bgCol);
}

void PluginWindowManager::updateTheme(bool isDark, COLORREF bgCol) {
    m_isDark = isDark;
    m_bgCol = bgCol;

    HBRUSH oldBrush = m_bgBrush;
    m_bgBrush = CreateSolidBrush(bgCol);

    BOOL useDark = isDark ? TRUE : FALSE;
    for (const auto& [plugin, hwnd] : m_openWindows) {
        if (hwnd && IsWindow(hwnd)) {
            if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
                DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
            }
            SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(m_bgBrush));
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }
    if (oldBrush) {
        DeleteObject(oldBrush);
    }
}

LRESULT CALLBACK PluginWindowManager::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* plugin = reinterpret_cast<IPluginInstance*>(GetPropW(hwnd, L"PluginInstance"));

    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = PluginWindowManager::instance().backgroundBrush();
            if (br) {
                FillRect(hdc, &rc, br);
            }
            return 1;
        }
        case WM_SIZE: {
            if (plugin && wParam != SIZE_MINIMIZED) {
                plugin->onGuiResize( static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)) );
            }
            break;
        }
        case WM_USER + 201: {
            int width = static_cast<int>(wParam);
            int height = static_cast<int>(lParam);
            if (width > 0 && height > 0) {
                RECT wr = {0, 0, width, height};
                AdjustWindowRectEx(&wr,
                                   static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)),
                                   FALSE,
                                   static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
                SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_SIZING: {
            if (plugin) {
                RECT* rc = reinterpret_cast<RECT*>(lParam);
                RECT border = {0, 0, 0, 0};
                DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
                DWORD exStyle = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE));
                AdjustWindowRectEx(&border, style, FALSE, exStyle);
                int borderWidth = (border.right - border.left);
                int borderHeight = (border.bottom - border.top);

                int clientW = (rc->right - rc->left) - borderWidth;
                int clientH = (rc->bottom - rc->top) - borderHeight;
                if (clientW > 0 && clientH > 0) {
                    int adjW = clientW;
                    int adjH = clientH;
                    if (plugin->adjustSize(adjW, adjH)) {
                        int finalWinW = adjW + borderWidth;
                        int finalWinH = adjH + borderHeight;
                        switch (wParam) {
                            case WMSZ_LEFT:
                            case WMSZ_TOPLEFT:
                            case WMSZ_BOTTOMLEFT:
                                rc->left = rc->right - finalWinW;
                                break;
                            default:
                                rc->right = rc->left + finalWinW;
                                break;
                        }
                        switch (wParam) {
                            case WMSZ_TOP:
                            case WMSZ_TOPLEFT:
                            case WMSZ_TOPRIGHT:
                                rc->top = rc->bottom - finalWinH;
                                break;
                            default:
                                rc->bottom = rc->top + finalWinH;
                                break;
                        }
                        return TRUE;
                    }
                }
            }
            break;
        }
        case WM_CLOSE: {
            if (plugin) {
                PluginWindowManager::instance().closePluginWindow(plugin);
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY: {
            RemovePropW(hwnd, L"PluginInstance");
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace praccy::plugins
