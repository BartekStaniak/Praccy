#pragma once

#include "plugin_base.h"
#include <windows.h>
#include <unordered_map>
#include <memory>
#include <string>

namespace praccy::plugins {

class PluginWindowManager {
public:
    static PluginWindowManager& instance();

    bool openPluginWindow(IPluginInstance* plugin);
    void closePluginWindow(IPluginInstance* plugin);
    void closeAllWindows();
    [[nodiscard]] bool isWindowOpen(IPluginInstance* plugin) const;
    [[nodiscard]] HWND getWindow(IPluginInstance* plugin) const;

private:
    PluginWindowManager();
    ~PluginWindowManager();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    ATOM m_windowClassAtom{0};
    std::unordered_map<IPluginInstance*, HWND> m_openWindows;
    std::unordered_map<HWND, IPluginInstance*> m_windowToPlugin;
};

} // namespace praccy::plugins
