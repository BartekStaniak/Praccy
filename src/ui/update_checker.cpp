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
    guards.maxCompressionRatio = 0; // Disabled for large binary update packages
    return praccy::utils::extractZipArchive(zipPath, destDir, outError, guards);
}

UpdateChecker& UpdateChecker::instance() {
    static UpdateChecker s_instance;
    return s_instance;
}

UpdateChecker::UpdateChecker() = default;

UpdateChecker::~UpdateChecker() = default;

UpdateInfo UpdateChecker::getInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_info;
}

static std::string extractJsonField(const std::string& json, const std::string& fieldName, size_t startOffset = 0) {
    std::string key = "\"" + fieldName + "\":";
    size_t pos = json.find(key, startOffset);
    if (pos == std::string::npos) return {};

    pos += key.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }

    if (pos >= json.length()) return {};

    if (json[pos] == '\"') {
        pos++;
        size_t end = pos;
        while (end < json.length()) {
            if (json[end] == '\"' && json[end - 1] != '\\') break;
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
        while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != ']') {
            end++;
        }
        return json.substr(pos, end - pos);
    }
}

static std::string httpGet(const std::string& url) {
    HINTERNET hInternet = InternetOpenA("Praccy-App/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return {};

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(),
        "User-Agent: Praccy-App/1.0\r\nAccept: application/vnd.github.v3+json\r\n", -1, flags, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return {};
    }

    std::string response;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
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

    std::thread([this, includeBeta]() {
        runCheck(includeBeta);
        m_isBusy.store(false);
    }).detach();
}

void UpdateChecker::runCheck(bool includeBeta) {
    UpdateInfo result;
    result.currentVersion = "v2.5-beta1";
    result.isBeta = includeBeta;

    // Helper lambda to parse release asset from a release JSON snippet
    auto parseReleaseAsset = [&](const std::string& relJson, UpdateInfo& target) {
        size_t assetPos = relJson.find("\"assets\":");
        if (assetPos != std::string::npos) {
            size_t zipPos = relJson.find(".zip\"", assetPos);
            if (zipPos != std::string::npos) {
                // Find beginning of asset object around zip
                size_t objStart = relJson.rfind('{', zipPos);
                if (objStart != std::string::npos && objStart >= assetPos) {
                    target.assetName = extractJsonField(relJson, "name", objStart);
                    target.assetUrl = extractJsonField(relJson, "browser_download_url", objStart);
                    std::string szStr = extractJsonField(relJson, "size", objStart);
                    if (!szStr.empty()) {
                        target.totalBytes = std::atoll(szStr.c_str());
                    }
                }
            }
        }
    };

    if (includeBeta) {
        // Query latest commit on 'dev' branch
        std::string json = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/commits/dev");
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
                result.downloadUrl = "https://github.com/BartekStaniak/Praccy/tree/dev";

                // Check if any release asset is available (e.g. latest release package)
                std::string allReleases = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/releases");
                if (!allReleases.empty()) {
                    parseReleaseAsset(allReleases, result);
                }

                result.status = UpdateStatus::UpdateAvailable;
            }
        }
    } else {
        // Query latest official release
        std::string json = httpGet("https://api.github.com/repos/BartekStaniak/Praccy/releases/latest");
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

                parseReleaseAsset(json, result);

                if (tag != result.currentVersion) {
                    result.status = UpdateStatus::UpdateAvailable;
                } else {
                    result.status = UpdateStatus::UpToDate;
                }
            }
        }
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

    std::thread([this]() {
        runDownload();
        m_isBusy.store(false);
    }).detach();
}

void UpdateChecker::runDownload() {
    std::string downloadUrl;
    std::string assetName;
    int64_t expectedSize = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        downloadUrl = m_info.assetUrl;
        assetName = m_info.assetName.empty() ? "Praccy-update.zip" : m_info.assetName;
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
        outFile.write(buffer, bytesRead);
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

    if (totalRead <= 1024) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Downloaded file is corrupted or incomplete.";
        return;
    }

    // Extract archive
    auto extractDir = updatesDir / "extracted";
    std::filesystem::remove_all(extractDir, ec);
    std::filesystem::create_directories(extractDir, ec);

    // In-process archive extraction replacing external shells
    std::string extractError;
    if (!extractZipArchive(zipPath, extractDir, extractError)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Failed to extract update package: " + extractError;
        return;
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
        // Cleanly exit current application process
        ExitProcess(0);
    }

    return false;
}

} // namespace praccy::ui
