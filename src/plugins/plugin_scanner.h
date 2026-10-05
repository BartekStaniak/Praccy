#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <thread>

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

enum class PluginSortMode {
    NameAscending = 0,
    NameDescending,
    VendorAscending,
    FormatAscending
};

class PluginScanner {
public:
    PluginScanner();
    ~PluginScanner();

    void scanAll();
    void scanSync();
    void addCustomSearchPath(const std::string& path);
    void removeCustomSearchPath(size_t index);
    [[nodiscard]] std::vector<std::string> searchPaths() const;

    [[nodiscard]] std::vector<PluginDescriptor> scannedPlugins() const;
    [[nodiscard]] size_t numPlugins() const;
    [[nodiscard]] bool isScanning() const noexcept { return m_isScanning.load(); }

    [[nodiscard]] bool isFavorite(const std::string& nameOrPath) const;
    void toggleFavorite(const std::string& nameOrPath);
    [[nodiscard]] size_t numFavorites() const;

    [[nodiscard]] std::vector<std::string> getDevelopers() const;
    [[nodiscard]] std::vector<PluginDescriptor> getFilteredPlugins(
        const std::string& searchQuery,
        const std::string& developerFilter,
        const std::string& formatFilter,
        PluginSortMode sortMode,
        bool favoritesOnly = false
    ) const;

private:
    void addDefaultPaths();
    void runScan();
    void scanDirectory(const std::filesystem::path& dirPath, std::vector<PluginDescriptor>& outPlugins);
    static std::string detectVendor(const std::filesystem::path& path, const std::string& name);

    std::vector<std::string> m_searchPaths;
    std::vector<PluginDescriptor> m_plugins;
    std::unordered_set<std::string> m_favorites;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_isScanning{false};
    std::atomic<bool> m_shouldStop{false};
    std::thread m_scanThread;
};

} // namespace praccy::plugins
