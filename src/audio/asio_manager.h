#pragma once

#include "asio_defs.h"
#include "audio_buffer.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

namespace praccy::audio {

struct AsioDriverDesc {
    std::string name;
    std::string description;
    std::string clsidString;
    CLSID clsid{};
};

struct AsioDriverInfo {
    std::string name;
    int32_t numInputChannels{0};
    int32_t numOutputChannels{0};
    int32_t minBufferSize{0};
    int32_t maxBufferSize{0};
    int32_t preferredBufferSize{0};
    int32_t currentBufferSize{0};
    double sampleRate{48000.0};
    std::vector<ASIOChannelInfo> inputChannels;
    std::vector<ASIOChannelInfo> outputChannels;
};

using AudioCallbackFn = std::function<void(const AudioBufferView& input, AudioBufferView& output)>;

class AsioManager {
public:
    AsioManager();
    ~AsioManager();

    [[nodiscard]] static std::vector<AsioDriverDesc> enumerateDrivers();

    bool loadDriver(const AsioDriverDesc& desc, HWND windowHandle = nullptr);
    void unloadDriver();

    bool start(int32_t bufferSize = -1);
    void stop();

    void openControlPanel();

    [[nodiscard]] bool isLoaded() const noexcept { return m_driver != nullptr; }
    [[nodiscard]] bool isRunning() const noexcept { return m_isRunning.load(std::memory_order_relaxed); }
    [[nodiscard]] double currentSampleRate() const noexcept { return m_atomicSampleRate.load(std::memory_order_relaxed); }
    [[nodiscard]] int32_t currentBufferSize() const noexcept { return m_atomicBufferSize.load(std::memory_order_relaxed); }
    [[nodiscard]] const AsioDriverInfo& driverInfo() const noexcept { return m_info; }

    bool setSampleRate(double sampleRate);
    bool setBufferSize(int32_t bufferSize);
    [[nodiscard]] bool canSampleRate(double sampleRate) const noexcept;
    bool checkHardwareChanges();

    void setAudioCallback(AudioCallbackFn callback) {
        if (!m_isRunning.load(std::memory_order_acquire)) {
            m_callback = std::move(callback);
        }
    }

    // Sample format conversion routines (public for real-time engine & unit tests)
    static void unpackSamples(ASIOSampleType type, const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples) noexcept;

    static void unpackInt24LSB(const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packInt24LSB(const float* src, void* dst, uint32_t numSamples) noexcept;

    static void unpackInt32LSB24(const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packInt32LSB24(const float* src, void* dst, uint32_t numSamples) noexcept;

    static void packInt32LSB(const float* src, int32_t* dst, uint32_t numSamples) noexcept;
    static void packFloat32LSB(const float* src, float* dst, uint32_t numSamples) noexcept;

private:
    static void bufferSwitchCallback(int32_t doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfoCallback(ASIOTime* params, int32_t doubleBufferIndex, ASIOBool directProcess);
    static void sampleRateDidChangeCallback(double sRate);
    static int32_t asioMessageCallback(int32_t selector, int32_t value, void* message, double* opt);

    void processAudio(int32_t doubleBufferIndex);

    static AsioManager* s_instance;

    IASIO* m_driver{nullptr};
    AsioDriverInfo m_info;
    std::vector<ASIOBufferInfo> m_bufferInfos;
    ASIOCallbacks m_callbacks{};
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_resetRequested{false};
    std::atomic<double> m_atomicSampleRate{48000.0};
    std::atomic<int32_t> m_atomicBufferSize{256};
    std::atomic<int32_t> m_pendingBufferSize{0};

    AudioCallbackFn m_callback;

    // Pre-allocated conversion buffers for zero-allocation realtime streaming
    OwnedAudioBuffer m_inputFloatBuffer;
    OwnedAudioBuffer m_outputFloatBuffer;
};

} // namespace praccy::audio
