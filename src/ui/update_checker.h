#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

namespace praccy::ui {

enum class UpdateStatus {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    ReadyToInstall,
    Error
};

struct UpdateInfo {
    UpdateStatus status{UpdateStatus::Idle};
    bool isBeta{false};
    std::string currentVersion{"v1.1.3"};
    std::string latestVersion;   // e.g. "v1.0.1" or "dev (f4610ff)"
    std::string releaseTitle;
    std::string releaseNotes;
    std::string downloadUrl;     // Browser link
    std::string assetUrl;        // Direct download link
    std::string assetName;       // e.g. "Praccy-v1.0.0-windows-x64.zip"
    int64_t totalBytes{0};
    int64_t downloadedBytes{0};
    float downloadProgress{0.0f}; // 0.0 to 1.0
    std::string errorMessage;
    std::string publishedDate;
    std::string localUpdatePath; // Path to extracted Praccy.exe
};

class UpdateChecker {
public:
    static UpdateChecker& instance();

    void checkForUpdates(bool includeBeta);
    void startDownload();
    bool applyUpdateAndRestart();
    UpdateInfo getInfo() const;

private:
    UpdateChecker();
    ~UpdateChecker();

    void runCheck(bool includeBeta);
    void runDownload();

    mutable std::mutex m_mutex;
    UpdateInfo m_info;
    std::atomic<bool> m_isBusy{false};
};

} // namespace praccy::ui
