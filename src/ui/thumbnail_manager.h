#pragma once

#include <d3d11.h>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <filesystem>
#include <cstdint>

namespace praccy::ui {

struct Thumbnail {
    ID3D11ShaderResourceView* srv{nullptr};
    int width{0};
    int height{0};
};

class ThumbnailManager {
public:
    static ThumbnailManager& instance();

    void init(ID3D11Device* device);
    void shutdown();

    // Look up or load a thumbnail for a plugin name
    Thumbnail* getThumbnail(const std::string& pluginName);

    // Request delayed capture from a window (e.g. 25 frames after opening to allow full paint)
    void requestCapture(const std::string& pluginName, HWND hwnd, int delayFrames = 25);

    // Immediately capture window content and update thumbnail
    bool captureWindow(const std::string& pluginName, HWND hwnd);

    // Load thumbnail from image file on disk
    bool loadFromFile(const std::string& pluginName, const std::filesystem::path& filePath);

    // Per-frame update to process delayed captures
    void update();

    // Rescan disk directories for thumbnail images
    void scanThumbnailFolders();

    // Clear and free cached textures
    void clearCache();

    // Helper to get app thumbnail storage directory
    static std::filesystem::path getThumbnailCacheDir();

private:
    ThumbnailManager();
    ~ThumbnailManager();

    std::string normalizeKey(const std::string& name) const;
    std::string sanitizeFilename(const std::string& name) const;
    bool uploadRgbaTexture(const std::string& key, int width, int height, const uint8_t* rgbaPixels);

    struct PendingCapture {
        std::string pluginName;
        HWND hwnd{nullptr};
        int framesRemaining{0};
    };

    ID3D11Device* m_device{nullptr};
    ULONG_PTR m_gdiplusToken{0};
    bool m_initialized{false};

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Thumbnail> m_cache;
    std::unordered_map<std::string, std::filesystem::path> m_discoveredFiles;
    std::vector<PendingCapture> m_pendingCaptures;
};

} // namespace praccy::ui
