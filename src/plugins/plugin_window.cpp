#include "plugin_window.h"
#include <iostream>

namespace praccy::plugins {

PluginWindowManager& PluginWindowManager::instance() {
    static PluginWindowManager s_instance;
    return s_instance;
}

PluginWindowManager::PluginWindowManager() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PluginWindowManager::windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PraccyPluginHostClass";

    m_windowClassAtom = RegisterClassExW(&wc);
}

PluginWindowManager::~PluginWindowManager() {
    closeAllWindows();
    if (m_windowClassAtom) {
        UnregisterClassW(reinterpret_cast<LPCWSTR>(m_windowClassAtom), GetModuleHandle(nullptr));
    }
}

bool PluginWindowManager::openPluginWindow(IPluginInstance* plugin) {
    if (!plugin) return false;

    // If window already open, bring to front
    auto it = m_openWindows.find(plugin);
    if (it != m_openWindows.end() && IsWindow(it->second)) {
        ShowWindow(it->second, SW_RESTORE);
        SetForegroundWindow(it->second);
        return true;
    }

    std::wstring wTitle;
    for (char c : plugin->name()) {
        wTitle.push_back(static_cast<wchar_t>(c));
    }
    wTitle += L" - Praccy";

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        reinterpret_cast<LPCWSTR>(m_windowClassAtom),
        wTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        850, 600,
        nullptr, nullptr,
        GetModuleHandle(nullptr),
        this
    );

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

    m_openWindows[plugin] = hwnd;
    m_windowToPlugin[hwnd] = plugin;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
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

LRESULT CALLBACK PluginWindowManager::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* plugin = reinterpret_cast<IPluginInstance*>(GetPropW(hwnd, L"PluginInstance"));

    switch (msg) {
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
