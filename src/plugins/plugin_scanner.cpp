#include "plugin_scanner.h"
#include <cstdlib>
#include <iostream>
#include <algorithm>

namespace praccy::plugins {

PluginScanner::PluginScanner() {
    addDefaultPaths();
    scanAll();
}

void PluginScanner::addDefaultPaths() {
    m_searchPaths.clear();

    // Standard 64-bit VST3 & CLAP directories on Windows
    m_searchPaths.push_back("C:\\Program Files\\Common Files\\VST3");
    m_searchPaths.push_back("C:\\Program Files\\Common Files\\CLAP");
    m_searchPaths.push_back("C:\\Program Files\\VstPlugins");
    m_searchPaths.push_back("C:\\Program Files\\Steinberg\\VstPlugins");

    // User LocalAppData locations
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp) {
        std::string lap(localApp);
        m_searchPaths.push_back(lap + "\\Programs\\Common\\VST3");
        m_searchPaths.push_back(lap + "\\Programs\\Common\\CLAP");
    }
}

void PluginScanner::addCustomSearchPath(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_searchPaths.begin(), m_searchPaths.end(), path) == m_searchPaths.end()) {
        m_searchPaths.push_back(path);
    }
}

void PluginScanner::removeCustomSearchPath(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_searchPaths.size()) {
        m_searchPaths.erase(m_searchPaths.begin() + index);
    }
}

void PluginScanner::scanAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isScanning = true;
    m_plugins.clear();

    // Register built-in effects
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Drive",
        .path = "builtin://drive",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Distortion"
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Amp Sim",
        .path = "builtin://amp",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Amp Emulation"
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Stereo Delay",
        .path = "builtin://delay",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Delay/Echo"
    });

    for (const auto& dirStr : m_searchPaths) {
        std::error_code ec;
        std::filesystem::path p(dirStr);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            scanDirectory(p);
        }
    }

    m_isScanning = false;
}

void PluginScanner::scanDirectory(const std::filesystem::path& dirPath) {
    std::error_code ec;
    for (auto iter = std::filesystem::recursive_directory_iterator(dirPath, std::filesystem::directory_options::skip_permission_denied, ec);
         iter != std::filesystem::recursive_directory_iterator(); ++iter) {
        if (ec) break;

        const auto& path = iter->path();
        const std::string ext = path.extension().string();

        if (ext == ".clap") {
            const std::string name = path.stem().string();
            auto itExists = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& d) {
                return d.name == name;
            });
            if (itExists == m_plugins.end()) {
                PluginDescriptor desc;
                desc.name = name;
                desc.path = path.string();
                desc.type = PluginType::CLAP;
                desc.vendor = "Third Party";
                desc.category = "CLAP";
                m_plugins.push_back(std::move(desc));
            }
        } else if (ext == ".vst3") {
            const std::string name = path.stem().string();
            auto itExists = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& d) {
                return d.name == name;
            });
            if (itExists == m_plugins.end()) {
                PluginDescriptor desc;
                desc.name = name;
                desc.path = path.string();
                desc.type = PluginType::VST3;
                desc.vendor = "Third Party";
                desc.category = "VST3";
                m_plugins.push_back(std::move(desc));
            }
            if (iter->is_directory()) {
                iter.disable_recursion_pending();
            }
        }
    }
}

} // namespace praccy::plugins
