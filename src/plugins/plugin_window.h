#pragma once

#include "plugin_base.h"
#if defined(_WIN32)
#include <windows.h>
#else
using ATOM = uint16_t;
using HBRUSH = void*;
using COLORREF = uint32_t;
using UINT = unsigned int;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef RGB
#define RGB(r,g,b) ((uint32_t)(((uint8_t)(r)|((uint16_t)((uint8_t)(g))<<8))|(((uint32_t)(uint8_t)(b))<<16)))
#endif
#endif
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <string>

namespace praccy::plugins {

class PluginWindowManager {
public:
    static PluginWindowManager& instance();

    void setMainWindow(HWND mainHwnd) noexcept { m_mainHwnd = mainHwnd; }
    bool openPluginWindow(IPluginInstance* plugin);
    bool openPluginWindowAsync(IPluginInstance* plugin, std::function<void(bool success)> onComplete = nullptr);
    void update();
    void closePluginWindow(IPluginInstance* plugin);
    void closeAllWindows();
    [[nodiscard]] bool isWindowOpen(IPluginInstance* plugin) const;
    [[nodiscard]] bool isWindowOpening(IPluginInstance* plugin) const;
    [[nodiscard]] HWND getWindow(IPluginInstance* plugin) const;
    void updateTitleBars(bool isDark);
    void updateTheme(bool isDark, COLORREF bgCol);
    [[nodiscard]] HBRUSH backgroundBrush() const noexcept { return m_bgBrush; }
    [[nodiscard]] COLORREF backgroundColor() const noexcept { return m_bgCol; }

private:
    PluginWindowManager();
    ~PluginWindowManager();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_mainHwnd{nullptr};
    ATOM m_windowClassAtom{0};
    bool m_isDark{true};
    HBRUSH m_bgBrush{nullptr};
    COLORREF m_bgCol{RGB(18, 19, 22)};
    std::unordered_map<IPluginInstance*, HWND> m_openWindows;
    std::unordered_map<HWND, IPluginInstance*> m_windowToPlugin;
    std::unordered_set<IPluginInstance*> m_openingPlugins;
    std::unordered_map<IPluginInstance*, HWND> m_pendingWindows;
};

} // namespace praccy::plugins
