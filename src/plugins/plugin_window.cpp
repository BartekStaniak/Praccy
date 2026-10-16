#include "plugin_window.h"
#include <iostream>

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

bool PluginWindowManager::openPluginWindow(IPluginInstance* plugin) {
    if (!plugin) return false;

    // Determine initial preferred size from plugin
    int prefW = 850;
    int prefH = 600;
    plugin->getPreferredSize(prefW, prefH);

    // If window already open, bring to front, ensure topmost, restore
    auto it = m_openWindows.find(plugin);
    if (it != m_openWindows.end() && IsWindow(it->second)) {
        HWND existingHwnd = it->second;
        ShowWindow(existingHwnd, SW_RESTORE);
        SetWindowPos(existingHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(existingHwnd);
        SetForegroundWindow(existingHwnd);
        return true;
    }

    std::wstring wTitle;
    for (char c : plugin->name()) {
        wTitle.push_back(static_cast<wchar_t>(c));
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
        if (HMODULE hDwm = LoadLibraryW(L"dwmapi.dll")) {
            typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
            FARPROC proc = GetProcAddress(hDwm, "DwmSetWindowAttribute");
            if (proc) {
                PFN_DwmSetWindowAttribute pfnSetAttr = nullptr;
                std::memcpy(&pfnSetAttr, &proc, sizeof(proc));
                BOOL useDark = m_isDark ? TRUE : FALSE;
                if (FAILED(pfnSetAttr(hwnd, 20, &useDark, sizeof(useDark)))) {
                    pfnSetAttr(hwnd, 19, &useDark, sizeof(useDark));
                }
            }
            FreeLibrary(hDwm);
        }
    }

    if (!hwnd) {
        std::cerr << "Failed to create Win32 window for plugin: " << plugin->name() << "\n";
        return false;
    }

    SetPropW(hwnd, L"PluginInstance", reinterpret_cast<HANDLE>(plugin));

    if (!plugin->openGui(hwnd)) {
        std::cerr << "plugin->openGui returned false for: " << plugin->name() << "\n";
        DestroyWindow(hwnd);
        return false;
    }

    // Check if plugin size changed after attaching GUI
    int actualW = prefW;
    int actualH = prefH;
    plugin->getPreferredSize(actualW, actualH);
    if (actualW != prefW || actualH != prefH) {
        RECT actualWr = {0, 0, actualW, actualH};
        AdjustWindowRectEx(&actualWr, style, FALSE, exStyle);
        winW = actualWr.right - actualWr.left;
        winH = actualWr.bottom - actualWr.top;
        posX = std::max(0, (screenW - winW) / 2);
        posY = std::max(0, (screenH - winH) / 2);
    }

    m_openWindows[plugin] = hwnd;
    m_windowToPlugin[hwnd] = plugin;

    // Position centered and on top
    SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, winW, winH, SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    return true;
}

void PluginWindowManager::closePluginWindow(IPluginInstance* plugin) {
    auto it = m_openWindows.find(plugin);
    if (it != m_openWindows.end()) {
        HWND hwnd = it->second;
        m_openWindows.erase(it);
        m_windowToPlugin.erase(hwnd);
        plugin->closeGui();
        DestroyWindow(hwnd);
    }
}

void PluginWindowManager::closeAllWindows() {
    for (auto& [plugin, hwnd] : m_openWindows) {
        if (plugin) plugin->closeGui();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
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

    if (m_bgBrush) {
        DeleteObject(m_bgBrush);
    }
    m_bgBrush = CreateSolidBrush(bgCol);

    if (HMODULE hDwm = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        FARPROC proc = GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (proc) {
            PFN_DwmSetWindowAttribute pfnSetAttr = nullptr;
            std::memcpy(&pfnSetAttr, &proc, sizeof(proc));
            BOOL useDark = isDark ? TRUE : FALSE;
            for (const auto& [plugin, hwnd] : m_openWindows) {
                if (hwnd && IsWindow(hwnd)) {
                    if (FAILED(pfnSetAttr(hwnd, 20, &useDark, sizeof(useDark)))) {
                        pfnSetAttr(hwnd, 19, &useDark, sizeof(useDark));
                    }
                    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(m_bgBrush));
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
        }
        FreeLibrary(hDwm);
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
