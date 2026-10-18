#pragma once

#include "../../plugins/plugin_scanner.h"
#include "../../audio/graph_engine.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace praccy::ui {

enum class PaletteScope : uint8_t {
    All = 0,
    Favorites = 1,
    Recents = 2
};

struct PaletteItem {
    const plugins::PluginDescriptor* descriptor{nullptr};
    std::string displayName;
    std::string categoryName;
    int score{0};
    bool isRecent{false};
    bool isFavorite{false};
};

class PluginBrowserModal {
public:
    PluginBrowserModal(plugins::PluginScanner& scanner, audio::GraphEngine& graph);
    ~PluginBrowserModal() = default;

    PluginBrowserModal(const PluginBrowserModal&) = delete;
    PluginBrowserModal& operator=(const PluginBrowserModal&) = delete;

    /// Open palette targeting an insertion slot (-1 for serial chain; >= 0 for parallel block/branch)
    void open(int targetBlockIndex = -1, int targetBranchIndex = -1);

    /// Close the palette dialog
    void close();

    /// Check if palette is currently open
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

    /// Navigate selection by delta (+1 down, -1 up)
    void navigateSelection(int delta);

    /// Scope navigation
    void setScope(PaletteScope scope);
    [[nodiscard]] PaletteScope scope() const noexcept { return m_currentScope; }
    void cycleScope();

    /// Favorites management
    void toggleFavorite(const std::string& pluginName);
    void toggleFavoriteForSelected();
    [[nodiscard]] bool isFavorite(const std::string& pluginName) const;
    [[nodiscard]] const std::vector<std::string>& favoritePlugins() const noexcept { return m_favoritePlugins; }

    /// Callback invoked when a plugin is instantiated and inserted
    void setOnPluginInserted(std::function<void(const std::string&)> callback) {
        m_onPluginInserted = std::move(callback);
    }

private:
    void updateFilteredList();
    void instantiateSelectedPlugin();
    void recordPluginUsage(const plugins::PluginDescriptor& desc);
    void renderPluginRow(int index, float rowH);

    static int SearchInputCallback(ImGuiInputTextCallbackData* data);

    plugins::PluginScanner& m_scanner;
    audio::GraphEngine& m_graph;
    std::function<void(const std::string&)> m_onPluginInserted;

    bool m_isOpen{false};
    bool m_justOpened{false};
    bool m_needsFocus{false};
    bool m_scrollSelectionIntoView{false};
    bool m_tabHandledThisFrame{false};
    bool m_filterDirty{false};

    PaletteScope m_currentScope{PaletteScope::All};
    char m_searchQuery[128]{0};
    char m_lastSearchQuery[128]{0};
    int m_selectedIndex{0};
    std::vector<PaletteItem> m_filteredItems;
    std::vector<plugins::PluginDescriptor> m_allPlugins;

    int m_targetBlockIndex{-1};
    int m_targetBranchIndex{-1};

    std::vector<std::string> m_favoritePlugins;
    std::vector<std::string> m_recentPlugins;
};

} // namespace praccy::ui
