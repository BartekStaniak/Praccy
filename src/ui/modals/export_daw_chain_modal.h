#pragma once

#include "../../state/scene_manager.h"
#include "../../audio/graph_engine.h"
#include <string>
#include <filesystem>

namespace praccy::ui {

class ExportDawChainModal {
public:
    ExportDawChainModal() = default;

    void open() noexcept {
        m_isOpen = true;
        m_targetFormatIdx = 0;
        m_exportFilename = "PraccyChain";
        m_statusText.clear();
    }

    void close() noexcept {
        m_isOpen = false;
    }

    [[nodiscard]] bool isOpen() const noexcept {
        return m_isOpen;
    }

    void render(state::SceneManager& sceneMgr, audio::GraphEngine& graph);

private:
    bool m_isOpen{false};
    int m_targetFormatIdx{0}; // 0 = Reaper (.RfxChain), 1 = Ableton (.adg), 2 = DAWproject (.dawproject), 3 = VST3 (.vstpreset)
    std::string m_exportFilename{"PraccyChain"};
    std::string m_exportDirectory;
    std::string m_statusText;
};

} // namespace praccy::ui
