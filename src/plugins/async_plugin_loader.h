#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_set>
#include <atomic>
#include <cstdint>

#include "plugin_base.h"
#include "plugin_scanner.h"

namespace praccy::audio {
class PluginSlot;
}

namespace praccy::plugins {

struct AsyncLoadTask {
    uint64_t id{0};
    audio::PluginSlot* targetSlot{nullptr};
    std::string path;
    PluginType type{PluginType::VST3};
    std::string name;
    std::string vendor;
    std::vector<uint8_t> initialState;
    double sampleRate{48000.0};
    uint32_t maxBlockSize{256};
    bool openGuiWhenDone{false};
    std::function<void(bool success)> completionCallback;
};

struct AsyncLoadResult {
    uint64_t id{0};
    audio::PluginSlot* targetSlot{nullptr};
    std::unique_ptr<IPluginInstance> instance{nullptr};
    bool success{false};
    std::string errorMessage;
    bool openGuiWhenDone{false};
    std::function<void(bool success)> completionCallback;
};

class AsyncPluginLoader {
public:
    static AsyncPluginLoader& instance();

    /// Initialize the background worker pool (typically 2 workers)
    void init(size_t numWorkers = 2);

    /// Shut down workers and cancel pending loads
    void shutdown();

    /// Queue asynchronous load of a plugin
    uint64_t enqueueLoad(
        audio::PluginSlot* slot,
        const std::string& path,
        PluginType type,
        const std::string& name = "",
        const std::string& vendor = "",
        std::vector<uint8_t> initialState = {},
        double sampleRate = 48000.0,
        uint32_t maxBlockSize = 256,
        bool openGuiWhenDone = false,
        std::function<void(bool success)> callback = nullptr
    );

    /// Cancel any pending or in-flight background load for a slot (e.g. when slot is deleted)
    void cancelSlot(audio::PluginSlot* slot);

    /// Cancel all pending background tasks
    void cancelAll();

    /// Check if a given slot currently has a background load pending
    [[nodiscard]] bool isSlotLoading(const audio::PluginSlot* slot) const;

    /// Number of active/queued background tasks
    [[nodiscard]] size_t pendingTaskCount() const;

    /// Poll and dispatch completed loads on the UI thread
    void update(std::function<void(
        audio::PluginSlot* slot,
        std::unique_ptr<IPluginInstance> instance,
        bool openGuiWhenDone
    )> onSlotReady);

private:
    AsyncPluginLoader();
    ~AsyncPluginLoader();

    void workerLoop();

    std::vector<std::thread> m_workers;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<AsyncLoadTask> m_taskQueue;

    mutable std::mutex m_resultMutex;
    std::vector<AsyncLoadResult> m_completedResults;

    mutable std::mutex m_cancelMutex;
    std::unordered_set<audio::PluginSlot*> m_cancelledSlots;
    std::unordered_set<audio::PluginSlot*> m_activeLoadingSlots;

    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_nextTaskId{1};
};

} // namespace praccy::plugins
