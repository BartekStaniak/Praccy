#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update_checker.h"
#include <windows.h>
#include <wininet.h>
#include <iostream>
#include <sstream>

namespace praccy::ui {

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

static std::string extractJsonField(const std::string& json, const std::string& fieldName) {
    std::string key = "\"" + fieldName + "\":";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return {};

    pos += key.length();
    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }

    if (pos >= json.length()) return {};

    if (json[pos] == '\"') {
        pos++; // skip opening quote
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
    }
    return {};
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
        return; // Already checking
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Checking;
        m_info.isBeta = includeBeta;
        m_info.errorMessage.clear();
    }

    std::thread([this, includeBeta]() {
        runCheck(includeBeta);
        m_isBusy.store(false);
    }).detach();
}

void UpdateChecker::runCheck(bool includeBeta) {
    UpdateInfo result;
    result.currentVersion = "v1.0.0";
    result.isBeta = includeBeta;

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
                // Get first line of message
                auto newline = message.find('\n');
                result.releaseTitle = (newline != std::string::npos) ? message.substr(0, newline) : message;
                result.publishedDate = date;
                result.downloadUrl = "https://github.com/BartekStaniak/Praccy/tree/dev";
                result.status = UpdateStatus::UpdateAvailable;
            }
        }
    } else {
        // Query latest release tag
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

} // namespace praccy::ui
