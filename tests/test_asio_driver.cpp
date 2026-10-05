#include <iostream>
#include <windows.h>
#include "audio/asio_manager.h"

int main() {
    std::cout << "Testing ASIO Drivers...\n";
    auto drivers = praccy::audio::AsioManager::enumerateDrivers();
    for (size_t i = 0; i < drivers.size(); ++i) {
        std::cout << "[" << i << "] " << drivers[i].name << " (" << drivers[i].clsidString << ")\n";
    }

    if (drivers.empty()) {
        std::cout << "No ASIO drivers found.\n";
        return 0;
    }

    praccy::audio::AsioManager asio;
    for (size_t i = 0; i < drivers.size(); ++i) {
        std::cout << "\nAttempting to load driver [" << i << "]: " << drivers[i].name << "...\n";
        bool loaded = asio.loadDriver(drivers[i], GetDesktopWindow());
        if (!loaded) {
            std::cout << "Failed to load " << drivers[i].name << "\n";
            continue;
        }

        const auto& info = asio.driverInfo();
        std::cout << "Loaded " << info.name << "\n";
        std::cout << "  Inputs: " << info.numInputChannels << ", Outputs: " << info.numOutputChannels << "\n";
        std::cout << "  Buffer sizes: min=" << info.minBufferSize << ", max=" << info.maxBufferSize << ", pref=" << info.preferredBufferSize << "\n";
        std::cout << "  Sample rate: " << info.sampleRate << "\n";

        for (int ch = 0; ch < info.numInputChannels; ++ch) {
            std::cout << "    In " << ch << ": " << info.inputChannels[ch].name << " (type " << info.inputChannels[ch].type << ")\n";
        }
        for (int ch = 0; ch < info.numOutputChannels; ++ch) {
            std::cout << "    Out " << ch << ": " << info.outputChannels[ch].name << " (type " << info.outputChannels[ch].type << ")\n";
        }

        std::cout << "Attempting to start driver...\n";
        bool started = asio.start();
        std::cout << "Started: " << (started ? "YES" : "NO") << "\n";

        if (started) {
            Sleep(500);
            asio.stop();
            std::cout << "Stopped cleanly.\n";
        }
        asio.unloadDriver();
    }

    return 0;
}
