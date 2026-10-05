#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "thumbnail_manager.h"
#include "../state/app_config.h"

#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <vector>

namespace praccy::ui {

// Helper: Standard CLSID for GDI+ PNG encoder
static const CLSID s_pngClsid = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

ThumbnailManager& ThumbnailManager::instance() {
    static ThumbnailManager s_instance;
    return s_instance;
}

ThumbnailManager::ThumbnailManager() = default;

ThumbnailManager::~ThumbnailManager() {
    shutdown();
}

void ThumbnailManager::init(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device = device;

    if (!m_initialized) {
        Gdiplus::GdiplusStartupInput gpsi;
        Gdiplus::Status status = Gdiplus::GdiplusStartup(&m_gdiplusToken, &gpsi, nullptr);
        if (status == Gdiplus::Ok) {
            m_initialized = true;
        } else {
            std::cerr << "ThumbnailManager: Failed to initialize GDI+ (status " << status << ")\n";
        }
    }

    scanThumbnailFolders();
}

void ThumbnailManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    clearCache();

    if (m_initialized) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
        m_initialized = false;
    }
    m_device = nullptr;
}

void ThumbnailManager::clearCache() {
    for (auto& [key, thumb] : m_cache) {
        if (thumb.srv) {
            thumb.srv->Release();
            thumb.srv = nullptr;
        }
    }
    m_cache.clear();
    m_pendingCaptures.clear();
}

std::filesystem::path ThumbnailManager::getThumbnailCacheDir() {
    auto dir = std::filesystem::path(state::AppConfig::getConfigDir()) / "thumbnails";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ThumbnailManager::normalizeKey(const std::string& name) const {
    std::string key;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::string ThumbnailManager::sanitizeFilename(const std::string& name) const {
    std::string clean;
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            clean.push_back('_');
        } else {
            clean.push_back(c);
        }
    }
    return clean;
}

void ThumbnailManager::scanThumbnailFolders() {
    m_discoveredFiles.clear();

    std::vector<std::filesystem::path> searchDirs;

    // 1. Current working directory
    searchDirs.push_back(std::filesystem::current_path() / "resources" / "thumbnails");
    searchDirs.push_back(std::filesystem::current_path() / "thumbnails");

    // 2. Executable parent directory
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        searchDirs.push_back(exeDir / "resources" / "thumbnails");
        searchDirs.push_back(exeDir / "thumbnails");
        searchDirs.push_back(exeDir / ".." / "resources" / "thumbnails");
    }

    // 3. User local app data cache
    searchDirs.push_back(getThumbnailCacheDir());

    for (const auto& dir : searchDirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;

            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                std::string stem = entry.path().stem().string();
                std::string key = normalizeKey(stem);
                if (!key.empty()) {
                    m_discoveredFiles[key] = entry.path();
                }
            }
        }
    }
}

bool ThumbnailManager::uploadRgbaTexture(const std::string& key, int width, int height, const uint8_t* rgbaPixels) {
    if (!m_device || width <= 0 || height <= 0 || !rgbaPixels) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA subData{};
    subData.pSysMem = rgbaPixels;
    subData.SysMemPitch = static_cast<UINT>(width * 4);
    subData.SysMemSlicePitch = 0;

    ID3D11Texture2D* pTexture = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &subData, &pTexture);
    if (FAILED(hr) || !pTexture) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* pSRV = nullptr;
    hr = m_device->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
    pTexture->Release();

    if (FAILED(hr) || !pSRV) return false;

    // Release old texture if exists
    auto it = m_cache.find(key);
    if (it != m_cache.end() && it->second.srv) {
        it->second.srv->Release();
    }

    m_cache[key] = Thumbnail{pSRV, width, height};
    return true;
}

bool ThumbnailManager::loadFromFile(const std::string& pluginName, const std::filesystem::path& filePath) {
    if (!m_initialized || !m_device) return false;

    std::wstring widePath = filePath.wstring();
    Gdiplus::Bitmap bmp(widePath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    int w = static_cast<int>(bmp.GetWidth());
    int h = static_cast<int>(bmp.GetHeight());
    if (w <= 0 || h <= 0) return false;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bmpData{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok) {
        return false;
    }

    std::vector<uint8_t> rgba(w * h * 4);
    const auto* pSrc = static_cast<const uint8_t*>(bmpData.Scan0);

    bool hasOpaqueAlpha = false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = pSrc + y * bmpData.Stride;
        uint8_t* dstRow = rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // R
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // B
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            if (srcRow[x * 4 + 3] > 0) hasOpaqueAlpha = true;
        }
    }
    bmp.UnlockBits(&bmpData);

    // If image alpha is entirely zero (common GDI artifact), force full opacity
    if (!hasOpaqueAlpha) {
        for (int i = 3; i < w * h * 4; i += 4) {
            rgba[i] = 255;
        }
    }

    std::string key = normalizeKey(pluginName);
    return uploadRgbaTexture(key, w, h, rgba.data());
}

Thumbnail* ThumbnailManager::getThumbnail(const std::string& pluginName) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string key = normalizeKey(pluginName);
    if (key.empty()) return nullptr;

    // 1. In-memory texture cache
    auto it = m_cache.find(key);
    if (it != m_cache.end() && it->second.srv) {
        return &it->second;
    }

    // 2. Exact match in discovered files
    auto fit = m_discoveredFiles.find(key);
    if (fit != m_discoveredFiles.end()) {
        if (loadFromFile(pluginName, fit->second)) {
            return &m_cache[key];
        }
    }

    // 3. Substring / fuzzy match in discovered files
    for (const auto& [fKey, path] : m_discoveredFiles) {
        if (key.find(fKey) != std::string::npos || fKey.find(key) != std::string::npos) {
            if (loadFromFile(pluginName, path)) {
                return &m_cache[key];
            }
        }
    }

    return nullptr;
}

void ThumbnailManager::requestCapture(const std::string& pluginName, HWND hwnd, int delayFrames) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!hwnd || !IsWindow(hwnd)) return;

    // If already scheduled for this window, let the existing countdown proceed
    for (const auto& pending : m_pendingCaptures) {
        if (pending.hwnd == hwnd) {
            return;
        }
    }
    m_pendingCaptures.push_back({pluginName, hwnd, delayFrames});
}

void ThumbnailManager::update() {
    std::vector<PendingCapture> ready;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pendingCaptures.begin(); it != m_pendingCaptures.end(); ) {
            it->framesRemaining--;
            if (it->framesRemaining <= 0) {
                ready.push_back(*it);
                it = m_pendingCaptures.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& cap : ready) {
        if (IsWindow(cap.hwnd)) {
            captureWindow(cap.pluginName, cap.hwnd);
        }
    }
}

bool ThumbnailManager::captureWindow(const std::string& pluginName, HWND hwnd) {
    if (!m_initialized || !m_device || !hwnd || !IsWindow(hwnd)) return false;

    // Use client area of the hosted window (contains exact plugin GUI)
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 32 || height <= 32) {
        GetWindowRect(hwnd, &rc);
        width = rc.right - rc.left;
        height = rc.bottom - rc.top;
    }

    if (width <= 32 || height <= 32) return false;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

    BOOL captured = FALSE;

    // Method 1: If window is visible on screen, capture directly from desktop DC
    // This captures 100% of GPU DirectComposition, OpenGL, JUCE, and VST3 windows!
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        POINT pt{0, 0};
        ClientToScreen(hwnd, &pt);
        if (BitBlt(hdcMem, 0, 0, width, height, hdcScreen, pt.x, pt.y, SRCCOPY)) {
            captured = TRUE;
        }
    }

    // Method 2: If BitBlt failed or window not on screen, try PrintWindow
    if (!captured) {
        HWND childHwnd = GetWindow(hwnd, GW_CHILD);
        if (childHwnd && IsWindow(childHwnd)) {
            captured = PrintWindow(childHwnd, hdcMem, 2 /*PW_RENDERFULLCONTENT*/);
            if (!captured) captured = PrintWindow(childHwnd, hdcMem, 0);
        }
        if (!captured) {
            captured = PrintWindow(hwnd, hdcMem, 2);
            if (!captured) captured = PrintWindow(hwnd, hdcMem, 0);
        }
    }

    Gdiplus::Bitmap fullBmp(hbm, nullptr);

    // Clean up GDI objects immediately
    SelectObject(hdcMem, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    if (fullBmp.GetLastStatus() != Gdiplus::Ok) return false;

    // Downscale smoothly to max width 480 (preserving aspect ratio)
    int targetW = width;
    int targetH = height;
    if (targetW > 480) {
        targetH = std::max(1, (targetH * 480) / targetW);
        targetW = 480;
    }

    Gdiplus::Bitmap scaledBmp(targetW, targetH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&scaledBmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&fullBmp, 0, 0, targetW, targetH);
    }

    // Extract RGBA buffer
    Gdiplus::Rect gRect(0, 0, targetW, targetH);
    Gdiplus::BitmapData bmpData{};
    if (scaledBmp.LockBits(&gRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok) {
        return false;
    }

    std::vector<uint8_t> rgba(targetW * targetH * 4);
    const auto* pSrc = static_cast<const uint8_t*>(bmpData.Scan0);

    bool hasContent = false;
    for (int y = 0; y < targetH; ++y) {
        const uint8_t* srcRow = pSrc + y * bmpData.Stride;
        uint8_t* dstRow = rgba.data() + y * targetW * 4;
        for (int x = 0; x < targetW; ++x) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = 255;
            if (r > 12 || g > 12 || b > 12) {
                hasContent = true;
            }
        }
    }
    scaledBmp.UnlockBits(&bmpData);

    // If the image was entirely black/blank, don't save or cache yet
    if (!hasContent) return false;

    // Save PNG to disk cache in AppData
    std::filesystem::path cacheDir = getThumbnailCacheDir();
    std::filesystem::path outPath = cacheDir / (sanitizeFilename(pluginName) + ".png");
    scaledBmp.Save(outPath.wstring().c_str(), &s_pngClsid, nullptr);

    // Update discovered files map and texture cache
    std::string key = normalizeKey(pluginName);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_discoveredFiles[key] = outPath;
        uploadRgbaTexture(key, targetW, targetH, rgba.data());
    }

    return true;
}

} // namespace praccy::ui
