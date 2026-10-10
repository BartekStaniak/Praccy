#pragma once

#include "../../plugins/plugin_scanner.h"
#include "../../audio/graph_engine.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <functional>

namespace praccy::ui {

struct PaletteItem {
    const plugins::PluginDescriptor* descriptor{nullptr};
    int score{0};
    bool isRecent{false};
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

    /// Callback invoked when a plugin is instantiated and inserted
    void setOnPluginInserted(std::function<void(const std::string&)> callback) {
        m_onPluginInserted = std::move(callback);
    }

private:
    void updateFilteredList();
    void instantiateSelectedPlugin();
    void recordPluginUsage(const plugins::PluginDescriptor& desc);

    static int SearchInputCallback(ImGuiInputTextCallbackData* data);

    plugins::PluginScanner& m_scanner;
    audio::GraphEngine& m_graph;
    std::function<void(const std::string&)> m_onPluginInserted;

    bool m_isOpen{false};
    bool m_needsFocus{false};
    bool m_scrollSelectionIntoView{false};

    char m_searchQuery[128]{0};
    char m_lastSearchQuery[128]{0};
    int m_selectedIndex{0};
    std::vector<PaletteItem> m_filteredItems;
    std::vector<plugins::PluginDescriptor> m_allPlugins;

    int m_targetBlockIndex{-1};
    int m_targetBranchIndex{-1};

    std::vector<std::string> m_recentPlugins;
};

} // namespace praccy::ui
