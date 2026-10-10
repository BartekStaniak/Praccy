#include "asio_manager.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <avrt.h>

namespace praccy::audio {

AsioManager* AsioManager::s_instance = nullptr;

AsioManager::AsioManager() {
    s_instance = this;
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
    m_info.currentBufferSize = m_info.preferredBufferSize;

    m_driver->getSampleRate(&m_info.sampleRate);

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
    if (m_isRunning.load()) return true;

    // Register MMCSS "Pro Audio" priority for calling control thread up front
    if (!m_mmcssHandle) {
        m_mmcssTaskIndex = 0;
        m_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);
    }

    if (bufferSize <= 0) {
        bufferSize = m_info.preferredBufferSize;
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
        if (m_mmcssHandle) {
            AvRevertMmThreadCharacteristics(m_mmcssHandle);
            m_mmcssHandle = nullptr;
            m_mmcssTaskIndex = 0;
        }
        return false;
    }

    m_inputFloatBuffer.resize(std::max(1, m_info.numInputChannels), bufferSize);
    m_outputFloatBuffer.resize(std::max(1, m_info.numOutputChannels), bufferSize);

    err = m_driver->start();
    if (err != ASE_OK) {
        std::cerr << "driver start failed with error: " << err << "\n";
        m_driver->disposeBuffers();
        if (m_mmcssHandle) {
            AvRevertMmThreadCharacteristics(m_mmcssHandle);
            m_mmcssHandle = nullptr;
            m_mmcssTaskIndex = 0;
        }
        return false;
    }

    m_isRunning.store(true);
    return true;
}

void AsioManager::stop() {
    if (!m_driver || !m_isRunning.load()) return;

    m_isRunning.store(false);
    m_driver->stop();
    m_driver->disposeBuffers();

    // Cleanly revert MMCSS association on the same calling control thread
    if (m_mmcssHandle) {
        AvRevertMmThreadCharacteristics(m_mmcssHandle);
        m_mmcssHandle = nullptr;
        m_mmcssTaskIndex = 0;
    }
}

void AsioManager::openControlPanel() {
    if (m_driver) {
        m_driver->controlPanel();
    }
}

void AsioManager::bufferSwitchCallback(int32_t doubleBufferIndex, ASIOBool directProcess) {
    if (s_instance) {
        s_instance->processAudio(doubleBufferIndex);
    }
}

ASIOTime* AsioManager::bufferSwitchTimeInfoCallback(ASIOTime* params, int32_t doubleBufferIndex, ASIOBool directProcess) {
    if (s_instance) {
        s_instance->processAudio(doubleBufferIndex);
    }
    return nullptr;
}

void AsioManager::sampleRateDidChangeCallback(double sRate) {
    if (s_instance) {
        s_instance->m_info.sampleRate = sRate;
    }
}

int32_t AsioManager::asioMessageCallback(int32_t selector, int32_t value, void* message, double* opt) {
    switch (selector) {
        case 1: // kAsioSelectorSupported
            if (value == 2) return 1; // kAsioEngineVersion
            return 0;
        case 2: // kAsioEngineVersion
            return 2; // Host ASIO version 2.0
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
            std::memcpy(dst, src, numSamples * sizeof(float));
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

void AsioManager::packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            auto* d = static_cast<int32_t*>(dst);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float sample = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int32_t>(sample * 2147483647.0f);
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            std::memcpy(dst, src, numSamples * sizeof(float));
            break;
        }
        case ASIOSTInt16LSB: {
            auto* d = static_cast<int16_t*>(dst);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float sample = std::clamp(src[i], -1.0f, 1.0f);
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

void AsioManager::processAudio(int32_t doubleBufferIndex) {
    if (!m_isRunning.load(std::memory_order_relaxed)) return;

    // Optional thread-local MMCSS registration on driver streaming thread (no cross-thread handle sharing)
    thread_local HANDLE tl_driverMmcss = nullptr;
    thread_local DWORD tl_driverTaskIdx = 0;
    if (!tl_driverMmcss) {
        tl_driverMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &tl_driverTaskIdx);
    }

    const uint32_t bufferSize = static_cast<uint32_t>(m_info.currentBufferSize);
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
