#include "async_plugin_loader.h"
#include "vst3_host.h"
#include "clap_host.h"
#include "builtin_dsp.h"
#include "../audio/graph_engine.h"

#if defined(_WIN32)
#include <objbase.h>
#endif
#include <iostream>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace praccy::plugins {

AsyncPluginLoader& AsyncPluginLoader::instance() {
    static AsyncPluginLoader s_instance;
    return s_instance;
}

AsyncPluginLoader::AsyncPluginLoader() {
    init(2);
}

AsyncPluginLoader::~AsyncPluginLoader() {
    shutdown();
}

void AsyncPluginLoader::init(size_t numWorkers) {
    if (m_running.load(std::memory_order_relaxed)) return;
    m_running.store(true, std::memory_order_release);

    for (size_t i = 0; i < numWorkers; ++i) {
        m_workers.emplace_back(&AsyncPluginLoader::workerLoop, this);
    }
}

void AsyncPluginLoader::shutdown() {
    if (!m_running.load(std::memory_order_relaxed)) return;
    m_running.store(false, std::memory_order_release);
    m_cv.notify_all();

    for (auto& w : m_workers) {
        if (w.joinable()) {
            w.join();
        }
    }
    m_workers.clear();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_taskQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        m_mainThreadTasks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_completedResults.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_latestSlotTaskId.clear();
        m_activeLoadingSlots.clear();
    }
}

uint64_t AsyncPluginLoader::enqueueLoad(
    audio::PluginSlot* slot,
    const std::string& path,
    PluginType type,
    const std::string& name,
    const std::string& vendor,
    std::vector<uint8_t> initialState,
    double sampleRate,
    uint32_t maxBlockSize,
    bool openGuiWhenDone,
    std::function<void(bool success)> callback,
    std::vector<std::pair<uint32_t, float>> initialParams
) {
    if (!m_running.load(std::memory_order_relaxed)) {
        init(2);
    }

    const uint64_t taskId = m_nextTaskId.fetch_add(1, std::memory_order_relaxed);

    AsyncLoadTask task;
    task.id = taskId;
    task.targetSlot = slot;
    task.path = path;
    task.type = type;
    task.name = name;
    task.vendor = vendor;
    task.initialState = std::move(initialState);
    task.initialParams = std::move(initialParams);
    task.sampleRate = sampleRate;
    task.maxBlockSize = maxBlockSize;
    task.openGuiWhenDone = openGuiWhenDone;
    task.completionCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        if (slot) {
            m_latestSlotTaskId[slot] = taskId;
            m_activeLoadingSlots.insert(slot);
        }
    }

    // CLAP specification requires plugin creation, initialization, and lifecycle to run on the [main-thread].
    // Run CLAP plugin loads on the UI/main thread via update() to prevent thread affinity crashes with JUCE MessageManager.
    if (task.type == PluginType::CLAP || task.path.find(".clap") != std::string::npos) {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        m_mainThreadTasks.push_back(std::move(task));
    } else {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_taskQueue.push_back(std::move(task));
        m_cv.notify_one();
    }

    return taskId;
}

void AsyncPluginLoader::cancelSlot(audio::PluginSlot* slot) {
    if (!slot) return;
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_latestSlotTaskId.erase(slot);
        m_activeLoadingSlots.erase(slot);
    }
    {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        for (auto it = m_mainThreadTasks.begin(); it != m_mainThreadTasks.end(); ) {
            if (it->targetSlot == slot) {
                it = m_mainThreadTasks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AsyncPluginLoader::cancelAll() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_taskQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        m_mainThreadTasks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_completedResults.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelAllWatermark = m_nextTaskId.load(std::memory_order_relaxed);
        m_latestSlotTaskId.clear();
        m_activeLoadingSlots.clear();
    }
}

bool AsyncPluginLoader::isSlotLoading(const audio::PluginSlot* slot) const {
    if (!slot) return false;
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    return m_activeLoadingSlots.find(const_cast<audio::PluginSlot*>(slot)) != m_activeLoadingSlots.end();
}

size_t AsyncPluginLoader::pendingTaskCount() const {
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        count += m_taskQueue.size();
    }
    {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        count += m_mainThreadTasks.size();
    }
    return count;
}

void AsyncPluginLoader::workerLoop() {
    // Initialize COM on worker thread for VST3/system interop
#if defined(_WIN32)
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    while (m_running.load(std::memory_order_relaxed)) {
        AsyncLoadTask task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this]() {
                return !m_running.load(std::memory_order_relaxed) || !m_taskQueue.empty();
            });

            if (!m_running.load(std::memory_order_relaxed) && m_taskQueue.empty()) {
                break;
            }

            task = std::move(m_taskQueue.front());
            m_taskQueue.pop_front();
        }

        // Check if task was cancelled or superseded before starting load
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            if (task.id < m_cancelAllWatermark) {
                continue;
            }
            if (task.targetSlot) {
                auto it = m_latestSlotTaskId.find(task.targetSlot);
                if (it == m_latestSlotTaskId.end() || it->second != task.id) {
                    continue;
                }
            }
        }

        AsyncLoadResult result;
        result.id = task.id;
        result.targetSlot = task.targetSlot;
        result.name = task.name;
        result.type = task.type;
        result.openGuiWhenDone = task.openGuiWhenDone;
        result.completionCallback = std::move(task.completionCallback);

        std::unique_ptr<IPluginInstance> inst;
        DWORD exCode = 0;

        bool callOk = safeCallPlugin([&]() {
            if (task.type == PluginType::VST3 || task.path.find(".vst3") != std::string::npos) {
                inst = Vst3PluginInstance::loadFromFile(task.path);
            } else if (task.type == PluginType::CLAP || task.path.find(".clap") != std::string::npos) {
                inst = ClapPluginInstance::loadFromFile(task.path);
            } else {
                if (task.path == "builtin://drive" || task.name.find("Drive") != std::string::npos) {
                    inst = std::make_unique<OverdriveEffect>();
                } else if (task.path == "builtin://amp" || task.name.find("Amp") != std::string::npos) {
                    inst = std::make_unique<TubeAmpEffect>();
                } else if (task.path == "builtin://delay" || task.name.find("Delay") != std::string::npos) {
                    inst = std::make_unique<StereoDelayEffect>();
                } else {
                    inst = std::make_unique<OverdriveEffect>();
                }
            }

            if (inst) {
                inst->prepare(task.sampleRate, task.maxBlockSize);
                if (!task.initialState.empty()) {
                    inst->loadState(task.initialState);
                }
                for (const auto& [paramId, paramVal] : task.initialParams) {
                    if (std::isfinite(paramVal)) {
                        inst->setParameterValue(paramId, paramVal);
                    }
                }
            }
        }, &exCode);

        // Check if task was cancelled or superseded while loading in background
        bool wasCancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            if (task.id < m_cancelAllWatermark) {
                wasCancelled = true;
            } else if (task.targetSlot) {
                auto it = m_latestSlotTaskId.find(task.targetSlot);
                if (it == m_latestSlotTaskId.end() || it->second != task.id) {
                    wasCancelled = true;
                }
            }
        }

        if (wasCancelled) {
            // Cleanly destroy plugin instance on worker thread
            inst.reset();
            continue;
        }

        if (callOk && inst) {
            result.success = true;
            result.instance = std::move(inst);
        } else {
            result.success = false;
            result.errorMessage = getExceptionDescription(exCode);
        }

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_completedResults.push_back(std::move(result));
        }
    }

#if defined(_WIN32)
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
#endif
}

void AsyncPluginLoader::update(std::function<void(
    audio::PluginSlot* slot,
    std::unique_ptr<IPluginInstance> instance,
    bool openGuiWhenDone
)> onSlotReady) {
    // 1. Process pending main-thread loads (e.g. CLAP plugins whose lifecycle requires execution on the main thread)
    std::deque<AsyncLoadTask> mainTasks;
    {
        std::lock_guard<std::mutex> lock(m_mainThreadMutex);
        if (!m_mainThreadTasks.empty()) {
            mainTasks.swap(m_mainThreadTasks);
        }
    }

    for (auto& task : mainTasks) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            if (task.id < m_cancelAllWatermark) {
                cancelled = true;
            } else if (task.targetSlot) {
                auto it = m_latestSlotTaskId.find(task.targetSlot);
                if (it == m_latestSlotTaskId.end() || it->second != task.id) {
                    cancelled = true;
                }
            }
        }

        if (cancelled) {
            if (task.completionCallback) task.completionCallback(false);
            continue;
        }

        std::unique_ptr<IPluginInstance> inst;
        DWORD exCode = 0;
        bool callOk = safeCallPlugin([&]() {
            inst = ClapPluginInstance::loadFromFile(task.path);
            if (inst) {
                inst->prepare(task.sampleRate, task.maxBlockSize);
                if (!task.initialState.empty()) {
                    inst->loadState(task.initialState);
                }
                for (const auto& [paramId, paramVal] : task.initialParams) {
                    if (std::isfinite(paramVal)) {
                        inst->setParameterValue(paramId, paramVal);
                    }
                }
            }
        }, &exCode);

        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            if (task.id < m_cancelAllWatermark) {
                cancelled = true;
            } else if (task.targetSlot) {
                auto it = m_latestSlotTaskId.find(task.targetSlot);
                if (it == m_latestSlotTaskId.end() || it->second != task.id) {
                    cancelled = true;
                } else {
                    m_latestSlotTaskId.erase(it);
                    m_activeLoadingSlots.erase(task.targetSlot);
                }
            }
        }

        if (cancelled) {
            inst.reset();
            if (task.completionCallback) task.completionCallback(false);
            continue;
        }

        if (callOk && inst && task.targetSlot) {
            if (onSlotReady) {
                onSlotReady(task.targetSlot, std::move(inst), task.openGuiWhenDone);
            }
            if (task.completionCallback) task.completionCallback(true);
        } else {
            if (task.targetSlot) {
                std::unique_ptr<audio::AudioNode> oldNode;
                std::string displayName = task.name.empty() ? "Failed to Load" : task.name;
                auto missing = std::make_unique<audio::MissingPluginPlaceholder>(
                    displayName,
                    "CLAP"
                );
                task.targetSlot->replaceInnerNode(std::move(missing), oldNode);
            }
            if (task.completionCallback) task.completionCallback(false);
        }
    }

    // 2. Process worker-thread completed results (VST3 and BuiltIn)
    std::vector<AsyncLoadResult> completed;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (m_completedResults.empty()) return;
        completed.swap(m_completedResults);
    }

    for (auto& res : completed) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            if (res.id < m_cancelAllWatermark) {
                cancelled = true;
            } else if (res.targetSlot) {
                auto it = m_latestSlotTaskId.find(res.targetSlot);
                if (it == m_latestSlotTaskId.end() || it->second != res.id) {
                    cancelled = true;
                } else {
                    m_latestSlotTaskId.erase(it);
                    m_activeLoadingSlots.erase(res.targetSlot);
                }
            }
        }

        if (cancelled) {
            res.instance.reset();
            if (res.completionCallback) res.completionCallback(false);
            continue;
        }

        if (res.success && res.instance && res.targetSlot) {
            auto* vst3 = dynamic_cast<Vst3PluginInstance*>(res.instance.get());
            if (vst3) {
                vst3->ensureControllerInitialized();
            }
            if (onSlotReady) {
                onSlotReady(res.targetSlot, std::move(res.instance), res.openGuiWhenDone);
            }
            if (res.completionCallback) res.completionCallback(true);
        } else {
            if (res.targetSlot) {
                std::unique_ptr<audio::AudioNode> oldNode;
                std::string displayName = res.name.empty() ? "Failed to Load" : res.name;
                auto missing = std::make_unique<audio::MissingPluginPlaceholder>(
                    displayName,
                    res.type == PluginType::CLAP ? "CLAP" : "VST3"
                );
                res.targetSlot->replaceInnerNode(std::move(missing), oldNode);
            }
            if (res.completionCallback) res.completionCallback(false);
        }
    }
}

} // namespace praccy::plugins
