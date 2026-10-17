#include "asio_manager.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <avrt.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace praccy::audio {

AsioManager* AsioManager::s_instance = nullptr;

AsioManager::AsioManager() {
    if (s_instance == nullptr) {
        s_instance = this;
    }
    CoInitialize(nullptr);
}

AsioManager::~AsioManager() {
    stop();
    unloadDriver();
    CoUninitialize();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

std::vector<AsioDriverDesc> AsioManager::enumerateDrivers() {
    std::vector<AsioDriverDesc> drivers;
    HKEY hAsioKey = nullptr;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hAsioKey) != ERROR_SUCCESS) {
        return drivers;
    }

    char subKeyName[256];
    DWORD subKeyIndex = 0;
    DWORD subKeyNameSize = sizeof(subKeyName);

    while (RegEnumKeyExA(hAsioKey, subKeyIndex, subKeyName, &subKeyNameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY hDriverKey = nullptr;
        if (RegOpenKeyExA(hAsioKey, subKeyName, 0, KEY_READ, &hDriverKey) == ERROR_SUCCESS) {
            char clsidStr[128] = {0};
            DWORD clsidSize = sizeof(clsidStr);
            char descStr[256] = {0};
            DWORD descSize = sizeof(descStr);

            RegQueryValueExA(hDriverKey, "CLSID", nullptr, nullptr, reinterpret_cast<LPBYTE>(clsidStr), &clsidSize);
            if (RegQueryValueExA(hDriverKey, "Description", nullptr, nullptr, reinterpret_cast<LPBYTE>(descStr), &descSize) != ERROR_SUCCESS) {
                std::snprintf(descStr, sizeof(descStr), "%s", subKeyName);
            }

            wchar_t wClsid[128] = {0};
            MultiByteToWideChar(CP_ACP, 0, clsidStr, -1, wClsid, 128);

            CLSID clsid;
            if (SUCCEEDED(CLSIDFromString(wClsid, &clsid))) {
                AsioDriverDesc desc;
                desc.name = subKeyName;
                desc.description = descStr;
                desc.clsidString = clsidStr;
                desc.clsid = clsid;
                drivers.push_back(std::move(desc));
            }
            RegCloseKey(hDriverKey);
        }
        subKeyIndex++;
        subKeyNameSize = sizeof(subKeyName);
    }

    RegCloseKey(hAsioKey);
    return drivers;
}

bool AsioManager::loadDriver(const AsioDriverDesc& desc, HWND windowHandle) {
    if (m_driver) {
        unloadDriver();
    }

    HRESULT hr = CoCreateInstance(desc.clsid, nullptr, CLSCTX_INPROC_SERVER, desc.clsid, reinterpret_cast<void**>(&m_driver));
    if (FAILED(hr) || !m_driver) {
        std::cerr << "Failed to instantiate ASIO driver COM object: " << desc.name << "\n";
        return false;
    }

    if (!m_driver->init(windowHandle)) {
        std::cerr << "ASIO driver init() returned false: " << desc.name << "\n";
        m_driver->Release();
        m_driver = nullptr;
        return false;
    }

    m_info.name = desc.name;
    int32_t inCh = 0, outCh = 0;
    m_driver->getChannels(&inCh, &outCh);
    m_info.numInputChannels = inCh;
    m_info.numOutputChannels = outCh;

    int32_t granularity = 0;
    m_driver->getBufferSize(&m_info.minBufferSize, &m_info.maxBufferSize, &m_info.preferredBufferSize, &granularity);
    if (m_info.preferredBufferSize <= 0) {
        m_info.preferredBufferSize = 256;
    }
    m_info.currentBufferSize = m_info.preferredBufferSize;

    m_driver->getSampleRate(&m_info.sampleRate);
    if (m_info.sampleRate <= 0.0) {
        m_info.sampleRate = 48000.0;
    }

    // Query channel info
    m_info.inputChannels.resize(inCh);
    for (int32_t i = 0; i < inCh; ++i) {
        m_info.inputChannels[i].channel = i;
        m_info.inputChannels[i].isInput = 1;
        m_driver->getChannelInfo(&m_info.inputChannels[i]);
    }

    m_info.outputChannels.resize(outCh);
    for (int32_t i = 0; i < outCh; ++i) {
        m_info.outputChannels[i].channel = i;
        m_info.outputChannels[i].isInput = 0;
        m_driver->getChannelInfo(&m_info.outputChannels[i]);
    }

    s_instance = this;
    return true;
}

void AsioManager::unloadDriver() {
    stop();
    if (m_driver) {
        m_driver->disposeBuffers();
        m_driver->Release();
        m_driver = nullptr;
    }
    m_info = AsioDriverInfo{};
}

bool AsioManager::start(int32_t bufferSize) {
    if (!m_driver) return false;

    if (bufferSize <= 0) {
        bufferSize = (m_info.currentBufferSize > 0) ? m_info.currentBufferSize
                   : (m_info.preferredBufferSize > 0 ? m_info.preferredBufferSize : 256);
    }
    if (m_info.minBufferSize > 0 && bufferSize < m_info.minBufferSize) {
        bufferSize = m_info.minBufferSize;
    }
    if (m_info.maxBufferSize > 0 && bufferSize > m_info.maxBufferSize) {
        bufferSize = m_info.maxBufferSize;
    }

    if (m_isRunning.load(std::memory_order_relaxed)) {
        if (m_info.currentBufferSize == bufferSize) {
            return true;
        }
        stop();
    }

    m_info.currentBufferSize = bufferSize;

    const int32_t totalChannels = m_info.numInputChannels + m_info.numOutputChannels;
    m_bufferInfos.resize(totalChannels);

    int32_t bufIdx = 0;
    for (int32_t i = 0; i < m_info.numInputChannels; ++i, ++bufIdx) {
        m_bufferInfos[bufIdx].isInput = 1;
        m_bufferInfos[bufIdx].channelNum = i;
        m_bufferInfos[bufIdx].buffers[0] = nullptr;
        m_bufferInfos[bufIdx].buffers[1] = nullptr;
    }

    for (int32_t i = 0; i < m_info.numOutputChannels; ++i, ++bufIdx) {
        m_bufferInfos[bufIdx].isInput = 0;
        m_bufferInfos[bufIdx].channelNum = i;
        m_bufferInfos[bufIdx].buffers[0] = nullptr;
        m_bufferInfos[bufIdx].buffers[1] = nullptr;
    }

    m_callbacks.bufferSwitch = &AsioManager::bufferSwitchCallback;
    m_callbacks.sampleRateDidChange = &AsioManager::sampleRateDidChangeCallback;
    m_callbacks.asioMessage = &AsioManager::asioMessageCallback;
    m_callbacks.bufferSwitchTimeInfo = &AsioManager::bufferSwitchTimeInfoCallback;

    ASIOError err = m_driver->createBuffers(m_bufferInfos.data(), totalChannels, bufferSize, &m_callbacks);
    if (err != ASE_OK) {
        std::cerr << "createBuffers failed with error: " << err << "\n";
        return false;
    }

    m_inputFloatBuffer.resize(std::max(1, m_info.numInputChannels), bufferSize);
    m_outputFloatBuffer.resize(std::max(1, m_info.numOutputChannels), bufferSize);

    m_atomicBufferSize.store(bufferSize, std::memory_order_release);
    m_atomicSampleRate.store(m_info.sampleRate, std::memory_order_release);

    err = m_driver->start();
    if (err != ASE_OK) {
        std::cerr << "driver start failed with error: " << err << "\n";
        m_driver->disposeBuffers();
        return false;
    }

    m_isRunning.store(true, std::memory_order_release);
    return true;
}

void AsioManager::stop() {
    if (!m_driver || !m_isRunning.load(std::memory_order_acquire)) return;

    m_isRunning.store(false, std::memory_order_release);
    m_driver->stop();
    m_driver->disposeBuffers();
}

void AsioManager::openControlPanel() {
    if (m_driver) {
        m_driver->controlPanel();
        checkHardwareChanges();
    }
}

bool AsioManager::setBufferSize(int32_t bufferSize) {
    if (!m_driver || bufferSize <= 0) return false;
    if (m_info.currentBufferSize == bufferSize && m_isRunning.load(std::memory_order_relaxed)) {
        return true;
    }
    bool wasRunning = m_isRunning.load(std::memory_order_relaxed);
    if (wasRunning) {
        stop();
    }
    m_info.currentBufferSize = bufferSize;
    if (wasRunning) {
        return start(bufferSize);
    }
    return true;
}

bool AsioManager::setSampleRate(double sampleRate) {
    if (!m_driver || sampleRate <= 0.0) return false;
    if (m_driver->canSampleRate(sampleRate) != ASE_OK) return false;
    if (std::abs(m_info.sampleRate - sampleRate) < 1.0) return true;

    bool wasRunning = m_isRunning.load(std::memory_order_relaxed);
    if (wasRunning) {
        stop();
    }
    ASIOError err = m_driver->setSampleRate(sampleRate);
    if (err == ASE_OK) {
        m_info.sampleRate = sampleRate;
    }
    if (wasRunning) {
        start(m_info.currentBufferSize);
    }
    return err == ASE_OK;
}

bool AsioManager::canSampleRate(double sampleRate) const noexcept {
    if (!m_driver || sampleRate <= 0.0) return false;
    return m_driver->canSampleRate(sampleRate) == ASE_OK;
}

bool AsioManager::checkHardwareChanges() {
    if (!m_driver) return false;

    bool changed = false;
    double currentSr = m_info.sampleRate;
    if (m_driver->getSampleRate(&currentSr) == ASE_OK && currentSr > 0.0) {
        if (std::abs(currentSr - m_info.sampleRate) > 1.0) {
            m_info.sampleRate = currentSr;
            changed = true;
        }
    }

    int32_t minSz = 0, maxSz = 0, prefSz = 0, gran = 0;
    if (m_driver->getBufferSize(&minSz, &maxSz, &prefSz, &gran) == ASE_OK) {
        m_info.minBufferSize = minSz;
        m_info.maxBufferSize = maxSz;
        if (prefSz > 0 && prefSz != m_info.preferredBufferSize) {
            m_info.preferredBufferSize = prefSz;
            m_info.currentBufferSize = prefSz;
            changed = true;
        }
    }

    if (m_resetRequested.exchange(false, std::memory_order_acq_rel)) {
        int32_t pending = m_pendingBufferSize.exchange(0, std::memory_order_acq_rel);
        if (pending > 0) {
            m_info.currentBufferSize = pending;
            m_info.preferredBufferSize = pending;
            m_atomicBufferSize.store(pending, std::memory_order_release);
        } else if (prefSz > 0 && m_info.currentBufferSize != prefSz) {
            m_info.currentBufferSize = prefSz;
            m_info.preferredBufferSize = prefSz;
            m_atomicBufferSize.store(prefSz, std::memory_order_release);
        }
        changed = true;
    }

    return changed;
}

void AsioManager::bufferSwitchCallback(int32_t doubleBufferIndex, ASIOBool /*directProcess*/) {
    if (s_instance) {
        s_instance->processAudio(doubleBufferIndex);
    }
}

ASIOTime* AsioManager::bufferSwitchTimeInfoCallback(ASIOTime* /*params*/, int32_t doubleBufferIndex, ASIOBool /*directProcess*/) {
    if (s_instance) {
        s_instance->processAudio(doubleBufferIndex);
    }
    return nullptr;
}

void AsioManager::sampleRateDidChangeCallback(double sRate) {
    if (s_instance && sRate > 0.0) {
        s_instance->m_info.sampleRate = sRate;
        s_instance->m_atomicSampleRate.store(sRate, std::memory_order_release);
        s_instance->m_resetRequested.store(true, std::memory_order_release);
    }
}

int32_t AsioManager::asioMessageCallback(int32_t selector, int32_t value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case 1: // kAsioSelectorSupported
            if (value == 2) return 1; // kAsioEngineVersion
            if (value == 3) return 1; // kAsioResetRequest
            if (value == 4) return 1; // kAsioBufferSizeChange
            if (value == 7) return 1; // kAsioSupportsTimeInfo
            return 0;
        case 2: // kAsioEngineVersion
            return 2; // Host ASIO version 2.0
        case 3: // kAsioResetRequest
            if (s_instance) {
                s_instance->m_resetRequested.store(true, std::memory_order_release);
            }
            return 1;
        case 4: // kAsioBufferSizeChange
            if (s_instance) {
                if (value > 0) {
                    s_instance->m_pendingBufferSize.store(value, std::memory_order_release);
                }
                s_instance->m_resetRequested.store(true, std::memory_order_release);
            }
            return 0; // Host requires asioResetRequest / buffer recreation
        case 7: // kAsioSupportsTimeInfo
            return 1;
        default:
            return 0;
    }
}

void AsioManager::unpackInt24LSB(const void* src, float* dst, uint32_t numSamples) noexcept {
    const auto* raw = static_cast<const uint8_t*>(src);
    constexpr float inv24 = 1.0f / 8388608.0f;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const uint32_t offset = s * 3;
        const uint32_t uval = (static_cast<uint32_t>(raw[offset]) << 8) |
                              (static_cast<uint32_t>(raw[offset + 1]) << 16) |
                              (static_cast<uint32_t>(raw[offset + 2]) << 24);
        const int32_t sample24 = static_cast<int32_t>(uval) >> 8;
        dst[s] = static_cast<float>(sample24) * inv24;
    }
}

void AsioManager::packInt24LSB(const float* src, void* dst, uint32_t numSamples) noexcept {
    auto* raw = static_cast<uint8_t*>(dst);

    for (uint32_t s = 0; s < numSamples; ++s) {
        float sample = src[s];
        if (std::isnan(sample)) sample = 0.0f;
        sample = std::clamp(sample, -1.0f, 1.0f);
        const int32_t sample24 = std::clamp(static_cast<int32_t>(std::round(sample * 8388608.0f)), -8388608, 8388607);

        const uint32_t offset = s * 3;
        raw[offset]     = static_cast<uint8_t>(sample24 & 0xFF);
        raw[offset + 1] = static_cast<uint8_t>((sample24 >> 8) & 0xFF);
        raw[offset + 2] = static_cast<uint8_t>((sample24 >> 16) & 0xFF);
    }
}

void AsioManager::unpackInt32LSB24(const void* src, float* dst, uint32_t numSamples) noexcept {
    const auto* raw = static_cast<const int32_t*>(src);
    constexpr float inv24 = 1.0f / 8388608.0f;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const uint32_t uval = static_cast<uint32_t>(raw[s]) << 8;
        const int32_t sample24 = static_cast<int32_t>(uval) >> 8;
        dst[s] = static_cast<float>(sample24) * inv24;
    }
}

void AsioManager::packInt32LSB24(const float* src, void* dst, uint32_t numSamples) noexcept {
    auto* raw = static_cast<int32_t*>(dst);

    for (uint32_t s = 0; s < numSamples; ++s) {
        float sample = src[s];
        if (std::isnan(sample)) sample = 0.0f;
        sample = std::clamp(sample, -1.0f, 1.0f);
        const int32_t sample24 = std::clamp(static_cast<int32_t>(std::round(sample * 8388608.0f)), -8388608, 8388607);
        raw[s] = sample24;
    }
}

void AsioManager::unpackSamples(ASIOSampleType type, const void* src, float* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            const auto* s = static_cast<const int32_t*>(src);
            constexpr float inv32 = 1.0f / 2147483648.0f;
            for (uint32_t i = 0; i < numSamples; ++i) dst[i] = static_cast<float>(s[i]) * inv32;
            break;
        }
        case ASIOSTFloat32LSB: {
            const auto* s = static_cast<const float*>(src);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float val = s[i];
                if (!std::isfinite(val)) val = 0.0f;
                dst[i] = val;
            }
            break;
        }
        case ASIOSTInt16LSB: {
            const auto* s = static_cast<const int16_t*>(src);
            constexpr float inv16 = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < numSamples; ++i) dst[i] = static_cast<float>(s[i]) * inv16;
            break;
        }
        case ASIOSTInt24LSB: {
            unpackInt24LSB(src, dst, numSamples);
            break;
        }
        case ASIOSTInt32LSB24: {
            unpackInt32LSB24(src, dst, numSamples);
            break;
        }
        default:
            std::memset(dst, 0, numSamples * sizeof(float));
            break;
    }
}

void AsioManager::packInt32LSB(const float* src, int32_t* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    for (uint32_t i = 0; i < numSamples; ++i) {
        float sample = src[i];
        if (!std::isfinite(sample)) sample = 0.0f;
        const double scaled = std::clamp(static_cast<double>(sample), -1.0, 1.0) * 2147483647.0;
        dst[i] = static_cast<int32_t>(std::lrint(scaled));
    }
}

void AsioManager::packFloat32LSB(const float* src, float* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    for (uint32_t i = 0; i < numSamples; ++i) {
        float s = src[i];
        if (!std::isfinite(s)) s = 0.0f;
        dst[i] = std::clamp(s, -1.0f, 1.0f);
    }
}

void AsioManager::packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            packInt32LSB(src, static_cast<int32_t*>(dst), numSamples);
            break;
        }
        case ASIOSTFloat32LSB: {
            packFloat32LSB(src, static_cast<float*>(dst), numSamples);
            break;
        }
        case ASIOSTInt16LSB: {
            auto* d = static_cast<int16_t*>(dst);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float sample = src[i];
                if (!std::isfinite(sample)) sample = 0.0f;
                sample = std::clamp(sample, -1.0f, 1.0f);
                d[i] = static_cast<int16_t>(sample * 32767.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            packInt24LSB(src, dst, numSamples);
            break;
        }
        case ASIOSTInt32LSB24: {
            packInt32LSB24(src, dst, numSamples);
            break;
        }
        default:
            break;
    }
}

namespace {

struct ThreadLocalMmcssGuard {
    HANDLE handle{nullptr};
    DWORD taskIndex{0};

    ThreadLocalMmcssGuard() {
        taskIndex = 0;
        handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    }

    ~ThreadLocalMmcssGuard() {
        if (handle) {
            AvRevertMmThreadCharacteristics(handle);
            handle = nullptr;
        }
    }

    ThreadLocalMmcssGuard(const ThreadLocalMmcssGuard&) = delete;
    ThreadLocalMmcssGuard& operator=(const ThreadLocalMmcssGuard&) = delete;
};

} // namespace

void AsioManager::processAudio(int32_t doubleBufferIndex) {
    if (!m_isRunning.load(std::memory_order_relaxed)) return;

    thread_local ThreadLocalMmcssGuard mmcssGuard;

#if defined(_M_X64) || defined(__x86_64__)
    // Denormal Prevention: FTZ (Flush-to-Zero) & DAZ (Denormals-Are-Zero)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif

    const uint32_t maxBuf = static_cast<uint32_t>(m_inputFloatBuffer.maxSamples());
    const int32_t reqSize = m_atomicBufferSize.load(std::memory_order_relaxed);
    const uint32_t bufferSize = (maxBuf > 0 && reqSize > 0)
        ? std::min<uint32_t>(static_cast<uint32_t>(reqSize), maxBuf)
        : static_cast<uint32_t>(std::max(0, reqSize));
    const int32_t inCh = m_info.numInputChannels;
    const int32_t outCh = m_info.numOutputChannels;

    auto inView = m_inputFloatBuffer.view(bufferSize);
    auto outView = m_outputFloatBuffer.view(bufferSize);

    // Convert hardware input buffers to 32-bit float
    for (int32_t i = 0; i < inCh; ++i) {
        const auto& chInfo = m_info.inputChannels[i];
        void* rawBuf = m_bufferInfos[i].buffers[doubleBufferIndex];
        float* dst = inView.channel(i);

        if (!rawBuf) {
            std::memset(dst, 0, bufferSize * sizeof(float));
            continue;
        }

        unpackSamples(chInfo.type, rawBuf, dst, bufferSize);
    }

    outView.clear();

    // Execute registered audio processing pipeline
    if (m_callback) {
        m_callback(inView, outView);
    }

    // Convert 32-bit float output back to hardware buffers
    for (int32_t i = 0; i < outCh; ++i) {
        const auto& chInfo = m_info.outputChannels[i];
        void* rawBuf = m_bufferInfos[inCh + i].buffers[doubleBufferIndex];
        const float* src = outView.channel(i);

        if (!rawBuf) continue;

        packSamples(chInfo.type, src, rawBuf, bufferSize);
    }

    m_driver->outputReady();
}

} // namespace praccy::audio
