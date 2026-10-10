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

#include "miniz.h"

namespace praccy::ui {

bool sanitizeZipEntryPath(const std::string& entryName, std::filesystem::path& outSafeRelativePath) {
    if (entryName.empty()) {
        return false;
    }

    // 1. Reject paths starting with / or \ (absolute paths)
    if (entryName.front() == '/' || entryName.front() == '\\') {
        return false;
    }

    // 2. Reject drive-qualified paths (e.g. "C:foo", "D:/bar")
    if (entryName.size() >= 2 && entryName[1] == ':') {
        return false;
    }

    // 3. Reject UNC network paths (e.g. "\\server\share" or "//server/share")
    if (entryName.rfind("\\\\", 0) == 0 || entryName.rfind("//", 0) == 0) {
        return false;
    }

    // 4. Normalize backslashes to forward slashes
    std::string normalized = entryName;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // 5. Tokenize path components by '/'
    std::vector<std::string> segments;
    std::string segment;
    std::stringstream ss(normalized);

    // List of reserved MS-DOS / Windows device names
    static const std::vector<std::string> reservedNames = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    while (std::getline(ss, segment, '/')) {
        if (segment.empty()) {
            continue; // Ignore redundant slashes like "a//b"
        }

        // Strict Zip Slip defense: disallow "." and ".." components
        if (segment == "." || segment == "..") {
            return false;
        }

        // Reject invalid Windows filename characters: < > : " | ? * or control characters
        for (char c : segment) {
            if (static_cast<unsigned char>(c) < 32 || c == '<' || c == '>' || c == ':' ||
                c == '"' || c == '|' || c == '?' || c == '*') {
                return false;
            }
        }

        // Reject trailing dots or spaces which Windows silently truncates
        if (segment.back() == '.' || segment.back() == ' ') {
            return false;
        }

        // Check against reserved Windows device names
        std::string upper = segment;
        auto dotPos = upper.find('.');
        if (dotPos != std::string::npos) {
            upper = upper.substr(0, dotPos);
        }
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        for (const auto& res : reservedNames) {
            if (upper == res) {
                return false;
            }
        }

        segments.push_back(segment);
    }

    if (segments.empty()) {
        return false;
    }

    // 6. Reconstruct sanitized relative path
    std::filesystem::path rel;
    for (const auto& seg : segments) {
        rel /= seg;
    }

    outSafeRelativePath = rel;
    return true;
}

bool extractZipArchive(const std::filesystem::path& zipPath,
                       const std::filesystem::path& destDir,
                       std::string& outError)
{
    std::error_code ec;
    if (!std::filesystem::exists(zipPath, ec) || !std::filesystem::is_regular_file(zipPath, ec)) {
        outError = "Archive file not found: " + zipPath.string();
        return false;
    }

    std::filesystem::create_directories(destDir, ec);
    auto canonicalDest = std::filesystem::weakly_canonical(destDir, ec);
    if (ec) {
        outError = "Failed to canonicalize destination directory: " + ec.message();
        return false;
    }

    // Open ZIP file with wide-character support on Windows
    FILE* fp = _wfopen(zipPath.wstring().c_str(), L"rb");
    if (!fp) {
        outError = "Failed to open zip archive: " + zipPath.string();
        return false;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        outError = "Corrupted or invalid ZIP archive.";
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    constexpr uint64_t MAX_SINGLE_FILE = 250ULL * 1024 * 1024;        // 250 MB
    constexpr uint64_t MAX_TOTAL_UNCOMPRESSED = 500ULL * 1024 * 1024; // 500 MB
    constexpr mz_uint MAX_FILE_COUNT = 10000;
    uint64_t totalUncompressed = 0;

    if (numFiles > MAX_FILE_COUNT) {
        mz_zip_reader_end(&zip);
        fclose(fp);
        outError = "ZIP archive contains too many entries (exceeds safe threshold).";
        return false;
    }

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Failed to read ZIP entry header at index " + std::to_string(i);
            return false;
        }

        std::string rawName = stat.m_filename;

        // Zip Bomb checks
        totalUncompressed += stat.m_uncomp_size;
        if (stat.m_uncomp_size > MAX_SINGLE_FILE || totalUncompressed > MAX_TOTAL_UNCOMPRESSED) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "ZIP entry exceeds safe uncompressed size limits (potential decompression bomb).";
            return false;
        }

        // Strict Zip Slip Sanitization
        std::filesystem::path safeRel;
        if (!sanitizeZipEntryPath(rawName, safeRel)) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Security violation: detected Zip Slip path traversal in entry: " + rawName;
            return false;
        }

        std::filesystem::path targetPath = canonicalDest / safeRel;
        auto canonicalTarget = std::filesystem::weakly_canonical(targetPath, ec);
        if (ec) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Failed to resolve destination path: " + targetPath.string();
            return false;
        }

        // Verify destination prefix containment
        auto destW = canonicalDest.wstring();
        auto targetW = canonicalTarget.wstring();
        if (targetW.compare(0, destW.size(), destW) != 0 ||
            (targetW.size() > destW.size() && targetW[destW.size()] != L'\\' && targetW[destW.size()] != L'/')) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Security violation: extracted entry escapes target directory: " + rawName;
            return false;
        }

        // Directory entry handling
        if (stat.m_is_directory || (!rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\'))) {
            std::filesystem::create_directories(canonicalTarget, ec);
            continue;
        }

        // Regular file extraction
        std::filesystem::create_directories(canonicalTarget.parent_path(), ec);

        size_t uncompSize = 0;
        void* pData = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
        if (!pData) {
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Failed to decompress file: " + rawName;
            return false;
        }

        std::ofstream outFile(canonicalTarget, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            mz_free(pData);
            mz_zip_reader_end(&zip);
            fclose(fp);
            outError = "Failed to write target file: " + canonicalTarget.string();
            return false;
        }

        if (uncompSize > 0) {
            outFile.write(reinterpret_cast<const char*>(pData), uncompSize);
        }
        outFile.close();
        mz_free(pData);
    }

    mz_zip_reader_end(&zip);
    fclose(fp);
    return true;
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
    result.currentVersion = "v1.1.3";
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
