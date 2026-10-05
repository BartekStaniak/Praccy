#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

namespace praccy::plugins {

enum class PluginType {
    BuiltIn,
    CLAP,
    VST3
};

struct PluginDescriptor {
    std::string name;
    std::string path;
    PluginType type{PluginType::BuiltIn};
    std::string vendor{"Unknown"};
    std::string category{"Effects"};

    [[nodiscard]] std::string typeString() const {
        switch (type) {
            case PluginType::BuiltIn: return "Built-In";
            case PluginType::CLAP: return "CLAP";
            case PluginType::VST3: return "VST3";
        }
        return "Unknown";
    }
};

class PluginScanner {
public:
    PluginScanner();

    void scanAll();
    void addCustomSearchPath(const std::string& path);
    void removeCustomSearchPath(size_t index);
    [[nodiscard]] const std::vector<std::string>& searchPaths() const noexcept { return m_searchPaths; }

    [[nodiscard]] const std::vector<PluginDescriptor>& scannedPlugins() const noexcept { return m_plugins; }
    [[nodiscard]] size_t numPlugins() const noexcept { return m_plugins.size(); }
    [[nodiscard]] bool isScanning() const noexcept { return m_isScanning; }

private:
    void addDefaultPaths();
    void scanDirectory(const std::filesystem::path& dirPath);

    std::vector<std::string> m_searchPaths;
    std::vector<PluginDescriptor> m_plugins;
    std::mutex m_mutex;
    bool m_isScanning{false};
};

} // namespace praccy::plugins
