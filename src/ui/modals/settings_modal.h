#pragma once

#include <vector>
#include <string>
#include <functional>
#include "../../audio/asio_manager.h"
#include "../../state/app_config.h"

namespace praccy::plugins {
    class PluginScanner;
}

namespace praccy::ui {

class SettingsModal {
public:
    SettingsModal(audio::AsioManager& asio,
                  plugins::PluginScanner& scanner,
                  std::function<void()> onOpenPluginBrowser = nullptr);
    ~SettingsModal();

    SettingsModal(const SettingsModal&) = delete;
    SettingsModal& operator=(const SettingsModal&) = delete;

    /// Open settings modal dialog
    void open();

    /// Close settings modal dialog
    void close();

    /// Check if settings modal is open
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

    /// Event handler for triggering external plugin browser
    void setOnOpenPluginBrowser(std::function<void()> callback) {
        m_onOpenPluginBrowser = std::move(callback);
    }

    /// Event handler for driver load / re-prepare
    void setOnDriverChanged(std::function<void(double, uint32_t)> callback) {
        m_onDriverChanged = std::move(callback);
    }

private:
    void renderAudioTab();
    void renderPluginsTab();
    void renderThemeTab();
    void renderUpdatesTab();
    void renderAboutTab();

    audio::AsioManager& m_asio;
    plugins::PluginScanner& m_scanner;
    std::function<void()> m_onOpenPluginBrowser;
    std::function<void(double, uint32_t)> m_onDriverChanged;

    bool m_isOpen{false};
    std::vector<audio::AsioDriverDesc> m_cachedDrivers;
    int m_selectedDriverIdx{0};
    state::AppConfig m_cachedConfig;
};

} // namespace praccy::ui
