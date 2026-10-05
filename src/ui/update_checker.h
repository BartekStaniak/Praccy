#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <atomic>

namespace praccy::ui {

enum class UpdateStatus {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Error
};

struct UpdateInfo {
    UpdateStatus status{UpdateStatus::Idle};
    bool isBeta{false};
    std::string currentVersion{"v1.0.0"};
    std::string latestVersion;   // e.g. "v1.0.1" or "dev (a0944a4)"
    std::string releaseTitle;
    std::string releaseNotes;
    std::string downloadUrl;
    std::string errorMessage;
    std::string publishedDate;
};

class UpdateChecker {
public:
    static UpdateChecker& instance();

    void checkForUpdates(bool includeBeta);
    UpdateInfo getInfo() const;

private:
    UpdateChecker();
    ~UpdateChecker();

    void runCheck(bool includeBeta);

    mutable std::mutex m_mutex;
    UpdateInfo m_info;
    std::atomic<bool> m_isBusy{false};
};

} // namespace praccy::ui
