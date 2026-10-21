#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update_checker.h"
#include "../state/app_config.h"

#include <windows.h>
#include <wininet.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

#include "../utils/archive_utils.h"

namespace praccy::ui {

bool sanitizeZipEntryPath(const std::string& entryName, std::filesystem::path& outSafeRelativePath) {
    return praccy::utils::sanitizeZipPath(entryName, outSafeRelativePath);
}

bool extractZipArchive(const std::filesystem::path& zipPath,
                       const std::filesystem::path& destDir,
                       std::string& outError)
{
    // Configure guards for application updater (executable bundles can exceed 30MB)
    praccy::utils::ArchiveBombGuards guards;
    guards.maxSingleFileSize = 250ULL * 1024 * 1024;
    guards.maxArchiveTotalSize = 500ULL * 1024 * 1024;
    guards.maxFileCount = 10000;
    guards.maxXmlSize = 30ULL * 1024 * 1024;
    guards.maxCompressionRatio = 100; // 100:1 ratio guard
    return praccy::utils::extractZipArchive(zipPath, destDir, outError, guards);
}

struct ParsedVersion {
    int major{0};
    int minor{0};
    int patch{0};
    int build{0};
    bool isPrerelease{false};
    int prereleaseNum{0};
};

static ParsedVersion parseVersion(std::string_view s) {
    ParsedVersion v;
    while (!s.empty() && (s.front() == 'v' || s.front() == 'V' || s.front() == ' ')) {
        s.remove_prefix(1);
    }

    size_t i = 0;
    auto readInt = [&]() -> int {
        int val = 0;
        bool hasDigits = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + (s[i] - '0');
            hasDigits = true;
            ++i;
        }
        return hasDigits ? val : 0;
    };

    v.major = readInt();
    if (i < s.size() && s[i] == '.') { ++i; v.minor = readInt(); }
    if (i < s.size() && s[i] == '.') { ++i; v.patch = readInt(); }
    if (i < s.size() && s[i] == '.') { ++i; v.build = readInt(); }

    if (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == 'b' || s[i] == 'a' || s[i] == 'r')) {
        v.isPrerelease = true;
        while (i < s.size()) {
            if (std::isdigit(static_cast<unsigned char>(s[i]))) {
                v.prereleaseNum = readInt();
                break;
            }
            ++i;
        }
    }
    return v;
}

bool isVersionNewer(std::string_view remoteVer, std::string_view currentVer) {
    if (remoteVer.empty()) return false;
    if (currentVer.empty()) return true;

    ParsedVersion r = parseVersion(remoteVer);
    ParsedVersion c = parseVersion(currentVer);

    if (r.major != c.major) return r.major > c.major;
    if (r.minor != c.minor) return r.minor > c.minor;
    if (r.patch != c.patch) return r.patch > c.patch;
    if (r.build != c.build) return r.build > c.build;

    if (!r.isPrerelease && c.isPrerelease) return true;
    if (r.isPrerelease && !c.isPrerelease) return false;

    if (r.isPrerelease && c.isPrerelease) {
        return r.prereleaseNum > c.prereleaseNum;
    }

    return false;
}

UpdateChecker& UpdateChecker::instance() {
    static UpdateChecker s_instance;
    return s_instance;
}

UpdateChecker::UpdateChecker() = default;

UpdateChecker::~UpdateChecker() {
    m_checkThread.request_stop();
    m_downloadThread.request_stop();
    if (m_checkThread.joinable()) {
        m_checkThread.join();
    }
    if (m_downloadThread.joinable()) {
        m_downloadThread.join();
    }
}

UpdateInfo UpdateChecker::getInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_info;
}

static std::string extractJsonField(const std::string& json, const std::string& fieldName, size_t startOffset = 0, size_t endOffset = std::string::npos) {
    std::string key = "\"" + fieldName + "\":";
    size_t pos = json.find(key, startOffset);
    if (pos == std::string::npos || (endOffset != std::string::npos && pos >= endOffset)) return {};

    pos += key.length();
    while (pos < json.length() && (endOffset == std::string::npos || pos < endOffset) &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }

    if (pos >= json.length() || (endOffset != std::string::npos && pos >= endOffset)) return {};

    if (json[pos] == '\"') {
        pos++;
        size_t end = pos;
        while (end < json.length() && (endOffset == std::string::npos || end < endOffset)) {
            if (json[end] == '\"') {
                size_t backslashes = 0;
                size_t k = end;
                while (k > pos && json[k - 1] == '\\') {
                    backslashes++;
                    k--;
                }
                if ((backslashes % 2) == 0) break;
            }
            end++;
        }
        std::string raw = json.substr(pos, end - pos);
        std::string out;
        for (size_t i = 0; i < raw.length(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.length()) {
                char next = raw[i + 1];
                if (next == 'n') { out += '\n'; i++; }
                else if (next == 'r') { i++; }
                else if (next == '\"') { out += '\"'; i++; }
                else if (next == '\\') { out += '\\'; i++; }
                else { out += raw[i]; }
            } else {
                out += raw[i];
            }
        }
        return out;
    } else {
        // Non-string (number / boolean)
        size_t end = pos;
        while (end < json.length() && (endOffset == std::string::npos || end < endOffset) &&
               json[end] != ',' && json[end] != '}' && json[end] != ']') {
            end++;
        }
        return json.substr(pos, end - pos);
    }
}

static std::string httpGet(const std::string& url) {
    HINTERNET hInternet = InternetOpenA("Praccy-App/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return {};

    DWORD timeoutMs = 10000;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(),
        "User-Agent: Praccy-App/1.0\r\nAccept: application/vnd.github.v3+json\r\n", -1, flags, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return {};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, nullptr) || statusCode != 200) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return {};
    }

    constexpr size_t kMaxResponseBytes = 2 * 1024 * 1024;
    std::string response;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (response.size() + bytesRead > kMaxResponseBytes) {
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
            return {};
        }
        response.append(buffer, bytesRead);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return response;
}

void UpdateChecker::checkForUpdates(bool includeBeta) {
    if (m_isBusy.exchange(true)) {
        return; // Already busy checking or downloading
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Checking;
        m_info.isBeta = includeBeta;
        m_info.errorMessage.clear();
        m_info.downloadProgress = 0.0f;
        m_info.downloadedBytes = 0;
    }

    if (m_checkThread.joinable()) {
        m_checkThread.request_stop();
        m_checkThread.join();
    }

    m_checkThread = std::jthread([this, includeBeta](std::stop_token st) {
        runCheck(includeBeta, st);
        m_isBusy.store(false);
    });
}

void UpdateChecker::runCheck(bool includeBeta, std::stop_token st) {
    if (st.stop_requested()) {
        return;
    }

    UpdateInfo result;
    result.currentVersion = praccy::kVersionString;
    result.isBeta = includeBeta;

    // Helper lambda to parse release asset strictly within a specific release JSON snippet
    auto parseReleaseAsset = [&](const std::string& relJson, UpdateInfo& target, size_t startOffset = 0, size_t endOffset = std::string::npos) {
        if (endOffset == std::string::npos) {
            endOffset = relJson.size();
        }
        size_t assetPos = relJson.find("\"assets\":", startOffset);
        if (assetPos == std::string::npos || assetPos >= endOffset) {
            return;
        }

        size_t arrOpen = relJson.find('[', assetPos);
        if (arrOpen == std::string::npos || arrOpen >= endOffset) {
            return;
        }

        size_t arrClose = relJson.find(']', arrOpen);
        if (arrClose == std::string::npos || arrClose > endOffset) {
            arrClose = endOffset;
        }

        size_t cur = arrOpen;
        std::string bestName;
        std::string bestUrl;
        std::string bestDigest;
        int64_t bestSize = 0;
        bool foundZip = false;

        while (cur < arrClose) {
            size_t oStart = relJson.find('{', cur);
            if (oStart == std::string::npos || oStart >= arrClose) break;

            // Find matching '}' for this asset object
            size_t brace = 0;
            size_t oEnd = std::string::npos;
            for (size_t i = oStart; i < arrClose; ++i) {
                if (relJson[i] == '{') brace++;
                else if (relJson[i] == '}') {
                    brace--;
                    if (brace == 0) {
                        oEnd = i;
                        break;
                    }
                }
            }
            if (oEnd == std::string::npos) break;

            std::string aName = extractJsonField(relJson, "name", oStart, oEnd);
            std::string aUrl = extractJsonField(relJson, "browser_download_url", oStart, oEnd);
            std::string aDigest = extractJsonField(relJson, "digest", oStart, oEnd);
            std::string aSizeStr = extractJsonField(relJson, "size", oStart, oEnd);
            int64_t aSize = aSizeStr.empty() ? 0 : std::atoll(aSizeStr.c_str());

            bool isZip = (aName.size() >= 4 && _stricmp(aName.c_str() + aName.size() - 4, ".zip") == 0);
            bool isExe = (aName.size() >= 4 && _stricmp(aName.c_str() + aName.size() - 4, ".exe") == 0);

            if (isZip) {
                bestName = aName;
                bestUrl = aUrl;
                bestDigest = aDigest;
                bestSize = aSize;
                foundZip = true;
                break; // .zip is preferred; stop searching
            } else if (isExe && !foundZip) {
                bestName = aName;
                bestUrl = aUrl;
                bestDigest = aDigest;
                bestSize = aSize;
            }

            cur = oEnd + 1;
        }

        if (!bestUrl.empty()) {
            target.assetName = bestName;
            target.assetUrl = bestUrl;
            if (bestDigest.rfind("sha256:", 0) == 0) {
                target.expectedSha256 = bestDigest.substr(7);
            } else {
                target.expectedSha256 = bestDigest;
            }
            target.totalBytes = bestSize;
        }
    };

    if (includeBeta) {
        // 1. First check GitHub releases for any newer release (including pre-releases)
        std::string allReleases = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/releases");
        if (st.stop_requested()) return;

        bool foundNewerRelease = false;
        if (!allReleases.empty()) {
            size_t firstObj = allReleases.find('{');
            if (firstObj != std::string::npos) {
                size_t brace = 0;
                size_t firstObjEnd = std::string::npos;
                for (size_t i = firstObj; i < allReleases.size(); ++i) {
                    if (allReleases[i] == '{') brace++;
                    else if (allReleases[i] == '}') {
                        brace--;
                        if (brace == 0) {
                            firstObjEnd = i;
                            break;
                        }
                    }
                }

                if (firstObjEnd != std::string::npos) {
                    std::string tag = extractJsonField(allReleases, "tag_name", firstObj, firstObjEnd);
                    std::string name = extractJsonField(allReleases, "name", firstObj, firstObjEnd);
                    std::string htmlUrl = extractJsonField(allReleases, "html_url", firstObj, firstObjEnd);
                    std::string publishedAt = extractJsonField(allReleases, "published_at", firstObj, firstObjEnd);
                    std::string body = extractJsonField(allReleases, "body", firstObj, firstObjEnd);

                    if (!tag.empty() && isVersionNewer(tag, result.currentVersion)) {
                        foundNewerRelease = true;
                        result.latestVersion = tag;
                        result.releaseTitle = name.empty() ? tag : name;
                        result.downloadUrl = htmlUrl.empty() ? "https://github.com/BartekStaniak/Praccy/releases" : htmlUrl;
                        result.publishedDate = publishedAt;
                        result.releaseNotes = body;
                        parseReleaseAsset(allReleases, result, firstObj, firstObjEnd);
                        result.status = UpdateStatus::UpdateAvailable;
                    }
                }
            }
        }

        if (!foundNewerRelease) {
            // 2. No newer release found; query latest commit on 'dev' branch
            std::string json = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/commits/dev");
            if (st.stop_requested()) return;

            if (json.empty()) {
                result.status = UpdateStatus::Error;
                result.errorMessage = "Unable to connect to GitHub. Check your network connection.";
            } else {
                std::string sha = extractJsonField(json, "sha");
                std::string message = extractJsonField(json, "message");
                std::string date = extractJsonField(json, "date");

                if (sha.empty()) {
                    result.status = UpdateStatus::Error;
                    result.errorMessage = "Could not parse GitHub API dev response.";
                } else {
                    std::string shortSha = sha.substr(0, std::min<size_t>(7, sha.length()));
                    result.latestVersion = "dev (" + shortSha + ")";
                    auto newline = message.find('\n');
                    result.releaseTitle = (newline != std::string::npos) ? message.substr(0, newline) : message;
                    result.publishedDate = date;
                    result.downloadUrl = "https://github.com/BartekStaniak/Praccy/commit/" + sha;

                    // Git commits do NOT have prebuilt binary assets
                    result.assetUrl.clear();
                    result.assetName.clear();
                    result.expectedSha256.clear();
                    result.totalBytes = 0;

                    std::string currentCommit = praccy::kGitCommit;
                    if (!currentCommit.empty() && currentCommit != "dev" && currentCommit != "unknown" &&
                        shortSha == currentCommit.substr(0, std::min<size_t>(7, currentCommit.length()))) {
                        result.status = UpdateStatus::UpToDate;
                    } else {
                        result.status = UpdateStatus::UpdateAvailable;
                    }
                }
            }
        }
    } else {
        // Query latest official release on main
        std::string json = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/releases/latest");
        if (st.stop_requested()) {
            return;
        }
        if (json.empty()) {
            result.status = UpdateStatus::Error;
            result.errorMessage = "Unable to connect to GitHub. Check your network connection.";
        } else {
            std::string tag = extractJsonField(json, "tag_name");
            std::string name = extractJsonField(json, "name");
            std::string htmlUrl = extractJsonField(json, "html_url");
            std::string publishedAt = extractJsonField(json, "published_at");
            std::string body = extractJsonField(json, "body");

            if (tag.empty()) {
                result.status = UpdateStatus::Error;
                result.errorMessage = "Could not parse GitHub release information.";
            } else {
                result.latestVersion = tag;
                result.releaseTitle = name.empty() ? tag : name;
                result.downloadUrl = htmlUrl.empty() ? "https://github.com/BartekStaniak/Praccy/releases" : htmlUrl;
                result.publishedDate = publishedAt;
                result.releaseNotes = body;

                parseReleaseAsset(json, result, 0, json.size());

                if (isVersionNewer(tag, result.currentVersion)) {
                    result.status = UpdateStatus::UpdateAvailable;
                } else {
                    result.status = UpdateStatus::UpToDate;
                    result.assetUrl.clear();
                    result.assetName.clear();
                    result.expectedSha256.clear();
                    result.totalBytes = 0;
                }
            }
        }
    }

    if (st.stop_requested()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info = result;
    }
}

void UpdateChecker::startDownload() {
    if (m_isBusy.exchange(true)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_info.assetUrl.empty()) {
            m_info.status = UpdateStatus::Error;
            m_info.errorMessage = "No direct downloadable package found for this build. Please view on GitHub.";
            m_isBusy.store(false);
            return;
        }
        m_info.status = UpdateStatus::Downloading;
        m_info.downloadProgress = 0.0f;
        m_info.downloadedBytes = 0;
        m_info.errorMessage.clear();
    }

    if (m_downloadThread.joinable()) {
        m_downloadThread.request_stop();
        m_downloadThread.join();
    }

    m_downloadThread = std::jthread([this](std::stop_token st) {
        runDownload(st);
        m_isBusy.store(false);
    });
}

void UpdateChecker::runDownload(std::stop_token st) {
    if (st.stop_requested()) {
        return;
    }
    std::string downloadUrl;
    std::string assetName;
    std::string expectedSha256;
    int64_t expectedSize = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        downloadUrl = m_info.assetUrl;
        std::string rawName = m_info.assetName.empty() ? "Praccy-update.zip" : m_info.assetName;
        assetName = std::filesystem::path(rawName).filename().string();
        if (assetName.empty()) assetName = "Praccy-update.zip";
        expectedSha256 = m_info.expectedSha256;
        expectedSize = m_info.totalBytes;
    }

    auto updatesDir = std::filesystem::path(state::AppConfig::getConfigDir()) / "updates";
    std::error_code ec;
    std::filesystem::create_directories(updatesDir, ec);

    auto zipPath = updatesDir / assetName;
    std::ofstream outFile(zipPath, std::ios::binary);
    if (!outFile.is_open()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Could not create local download file: " + zipPath.string();
        return;
    }

    HINTERNET hInternet = InternetOpenA("Praccy-App/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Failed to initialize internet connection.";
        return;
    }

    DWORD timeoutMs = 15000;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, downloadUrl.c_str(),
        "User-Agent: Praccy-App/1.0\r\nAccept: application/octet-stream\r\n", -1, flags, 0);

    if (!hUrl) {
        InternetCloseHandle(hInternet);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Failed to connect to asset download server.";
        return;
    }

    // Verify HTTP 200 status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, nullptr)) {
        if (statusCode != 200) {
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = UpdateStatus::Error;
            m_info.errorMessage = "Download server returned HTTP status " + std::to_string(statusCode);
            return;
        }
    }

    // Try to get total content length if not known
    if (expectedSize <= 0) {
        char lenBuf[64] = {0};
        DWORD lenSize = sizeof(lenBuf);
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH, lenBuf, &lenSize, nullptr)) {
            expectedSize = std::atoll(lenBuf);
        }
    }

    char buffer[32768];
    DWORD bytesRead = 0;
    int64_t totalRead = 0;

    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (st.stop_requested()) {
            break;
        }
        outFile.write(buffer, bytesRead);
        if (!outFile.good()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = UpdateStatus::Error;
            m_info.errorMessage = "Disk write failure while downloading update package.";
            break;
        }
        totalRead += bytesRead;

        float progress = (expectedSize > 0) ? (static_cast<float>(totalRead) / static_cast<float>(expectedSize)) : 0.5f;
        progress = std::clamp(progress, 0.0f, 1.0f);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.downloadedBytes = totalRead;
            m_info.totalBytes = expectedSize;
            m_info.downloadProgress = progress;
        }
    }

    outFile.close();
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (st.stop_requested()) {
        std::filesystem::remove(zipPath, ec);
        return;
    }

    if (!outFile.good() || m_info.status == UpdateStatus::Error) {
        std::filesystem::remove(zipPath, ec);
        return;
    }

    if (totalRead <= 1024) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Downloaded file is corrupted or incomplete.";
        std::filesystem::remove(zipPath, ec);
        return;
    }

    // Verify SHA-256 digest of downloaded package using portable FIPS 180-4 SHA-256
    {
        auto rotr = [](uint32_t x, uint32_t n) noexcept { return (x >> n) | (x << (32 - n)); };
        static constexpr uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfc,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffa,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        };
        uint32_t H[8] = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        auto processBlock = [&](const uint8_t blk[64]) noexcept {
            uint32_t W[64];
            for (int i = 0; i < 16; ++i) {
                W[i] = (static_cast<uint32_t>(blk[i * 4]) << 24) |
                       (static_cast<uint32_t>(blk[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(blk[i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(blk[i * 4 + 3]));
            }
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
                uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
                W[i] = W[i - 16] + s0 + W[i - 7] + s1;
            }
            uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ ((~e) & g);
                uint32_t temp1 = h + S1 + ch + K[i] + W[i];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = S0 + maj;
                h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
            }
            H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
        };

        std::ifstream inZip(zipPath, std::ios::binary);
        uint8_t blk[64];
        uint64_t bitLen = 0;
        while (inZip) {
            if (st.stop_requested()) {
                inZip.close();
                std::filesystem::remove(zipPath, ec);
                return;
            }
            inZip.read(reinterpret_cast<char*>(blk), 64);
            std::streamsize got = inZip.gcount();
            if (got == 64) {
                processBlock(blk);
                bitLen += 512;
            } else {
                bitLen += static_cast<uint64_t>(got) * 8u;
                blk[got++] = 0x80u;
                if (got > 56) {
                    while (got < 64) blk[got++] = 0;
                    processBlock(blk);
                    got = 0;
                }
                while (got < 56) blk[got++] = 0;
                for (int i = 0; i < 8; ++i) {
                    blk[56 + i] = static_cast<uint8_t>((bitLen >> ((7 - i) * 8)) & 0xFFu);
                }
                processBlock(blk);
                break;
            }
        }
        char hexDigest[65] = {0};
        for (int i = 0; i < 8; ++i) {
            std::snprintf(hexDigest + i * 8, 9, "%08x", H[i]);
        }
        if (!expectedSha256.empty()) {
            std::string expLower = expectedSha256;
            for (char& ch : expLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (expLower != hexDigest) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_info.status = UpdateStatus::Error;
                m_info.errorMessage = "SHA-256 verification failed for downloaded update package.";
                std::filesystem::remove(zipPath, ec);
                return;
            }
        }
    }

    if (st.stop_requested()) {
        std::filesystem::remove(zipPath, ec);
        return;
    }

    // Extract archive or stage executable
    auto extractDir = updatesDir / "extracted";
    std::filesystem::remove_all(extractDir, ec);
    std::filesystem::create_directories(extractDir, ec);

    bool isExe = (assetName.size() >= 4 &&
                  _stricmp(assetName.c_str() + assetName.size() - 4, ".exe") == 0);
    if (isExe) {
        // Direct executable asset: copy directly to extracted/Praccy.exe
        std::filesystem::copy_file(zipPath, extractDir / "Praccy.exe",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = UpdateStatus::Error;
            m_info.errorMessage = "Failed to stage update executable: " + ec.message();
            return;
        }
    } else {
        // In-process archive extraction replacing external shells
        std::string extractError;
        if (!extractZipArchive(zipPath, extractDir, extractError)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = UpdateStatus::Error;
            m_info.errorMessage = "Failed to extract update package: " + extractError;
            return;
        }
    }

    // Locate the extracted Praccy.exe
    std::filesystem::path foundExe;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(extractDir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() == "Praccy.exe") {
            foundExe = entry.path();
            break;
        }
    }

    if (foundExe.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Extracted update archive does not contain Praccy.exe.";
        return;
    }

    if (st.stop_requested()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.localUpdatePath = foundExe.string();
        m_info.downloadProgress = 1.0f;
        m_info.status = UpdateStatus::ReadyToInstall;
    }
}

bool UpdateChecker::applyUpdateAndRestart() {
    std::string newExePath;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        newExePath = m_info.localUpdatePath;
    }

    if (newExePath.empty() || !std::filesystem::exists(newExePath)) {
        return false;
    }

    wchar_t currentExe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH) == 0) {
        return false;
    }

    std::error_code ec;
    if (std::filesystem::equivalent(newExePath, currentExe, ec)) {
        return false; // Prevent overwriting self if paths are identical
    }

    DWORD currentPid = GetCurrentProcessId();
    std::wstring wNew = std::filesystem::path(newExePath).wstring();
    std::wstring wCur = currentExe;

    // Launch extracted Praccy.exe in --apply-update mode
    // Command line format: "<newExe>" --apply-update <pid> "<destExe>"
    std::wstring cmdLine = L"\"" + wNew + L"\" --apply-update " + std::to_wstring(currentPid) + L" \"" + wCur + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        wNew.c_str(),
        cmdLine.data(),
        nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr, &si, &pi
    );

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        // Post WM_CLOSE to main application window rather than abrupt ExitProcess while audio may be streaming
        HWND hMain = GetActiveWindow();
        if (!hMain) hMain = FindWindowW(nullptr, L"Praccy");
        if (hMain) {
            PostMessageW(hMain, WM_CLOSE, 0, 0);
        } else {
            ExitProcess(0);
        }
        return true;
    }

    return false;
}

} // namespace praccy::ui
