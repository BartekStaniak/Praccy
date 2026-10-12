#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "archive_utils.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cctype>

#include "miniz.h"

namespace praccy::utils {

bool sanitizeZipPath(std::string_view entryName, std::filesystem::path& outSafeRelativePath) {
    if (entryName.empty()) {
        return false;
    }

    // Reject null bytes anywhere
    if (entryName.find('\0') != std::string_view::npos) {
        return false;
    }

    // 1. Reject paths starting with / or \ (absolute paths)
    if (entryName.front() == '/' || entryName.front() == '\\') {
        return false;
    }

    // 2. Reject drive-qualified paths (e.g. "C:foo", "D:/bar") or colon anywhere
    if (entryName.find(':') != std::string_view::npos) {
        return false;
    }

    // 3. Reject UNC network paths (e.g. "\\server\share" or "//server/share" or "\\?\")
    if (entryName.rfind("\\\\", 0) == 0 || entryName.rfind("//", 0) == 0) {
        return false;
    }

    // 4. Normalize backslashes to forward slashes
    std::string normalized(entryName);
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

        // Reject invalid Windows filename characters: < > : " | ? * or control characters (< 32)
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

        // Check against reserved Windows device names (check base name before any dot)
        std::string upper = segment;
        auto dotPos = upper.find('.');
        if (dotPos != std::string::npos) {
            upper = upper.substr(0, dotPos);
        }
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(::toupper(c));
        });
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

namespace {

bool extractZipArchiveCore(mz_zip_archive& zip,
                           const std::filesystem::path& destDir,
                           std::string& outError,
                           const ArchiveBombGuards& guards)
{
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    auto canonicalDest = std::filesystem::weakly_canonical(destDir, ec);
    if (ec) {
        outError = "Failed to canonicalize destination directory: " + ec.message();
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    if (numFiles > guards.maxFileCount) {
        outError = "Archive exceeds safe file count limit (maximum " +
                   std::to_string(guards.maxFileCount) + " files allowed, contains too many entries).";
        return false;
    }

    uint64_t totalUncompressed = 0;

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            outError = "Failed to read ZIP entry header at index " + std::to_string(i);
            return false;
        }

        std::string rawName = stat.m_filename;

        // 1. Compression ratio guard
        if (guards.maxCompressionRatio > 0) {
            if (stat.m_comp_size > 0) {
                if (stat.m_uncomp_size > static_cast<uint64_t>(guards.maxCompressionRatio) * stat.m_comp_size) {
                    outError = "ZIP entry compression ratio exceeds safe limit (100:1 ratio guard triggered, potential decompression bomb).";
                    return false;
                }
            } else if (stat.m_uncomp_size > 0) {
                outError = "ZIP entry compression ratio exceeds safe limit (zero compressed size, potential decompression bomb).";
                return false;
            }
        }

        // 2. XML decompression bomb check
        std::string lowerName = rawName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
            return static_cast<char>(::tolower(c));
        });
        const bool isXml = (lowerName.size() >= 4 && lowerName.compare(lowerName.size() - 4, 4, ".xml") == 0);
        if (isXml && stat.m_uncomp_size > guards.maxXmlSize) {
            outError = "ZIP XML entry exceeds 30MB uncompressed limit (potential decompression bomb and size limits exceeded).";
            return false;
        }

        // 3. Single file uncompressed size check
        if (stat.m_uncomp_size > guards.maxSingleFileSize) {
            outError = "ZIP entry exceeds safe uncompressed size limits (potential decompression bomb).";
            return false;
        }

        // 4. Cumulative total uncompressed size check
        totalUncompressed += stat.m_uncomp_size;
        if (totalUncompressed > guards.maxArchiveTotalSize) {
            outError = "ZIP archive total uncompressed size exceeds safe limits (potential decompression bomb, exceeds 128MB limit).";
            return false;
        }

        // 5. Strict Zip Slip Path Traversal Sanitization
        std::filesystem::path safeRel;
        if (!sanitizeZipPath(rawName, safeRel)) {
            outError = "Security violation: detected Zip Slip path traversal in entry: " + rawName;
            return false;
        }

        std::filesystem::path targetPath = canonicalDest / safeRel;
        auto canonicalTarget = std::filesystem::weakly_canonical(targetPath, ec);
        if (ec) {
            outError = "Failed to resolve destination path: " + targetPath.string();
            return false;
        }

        // Verify destination prefix containment
        auto destW = canonicalDest.wstring();
        auto targetW = canonicalTarget.wstring();
        if (targetW.compare(0, destW.size(), destW) != 0 ||
            (targetW.size() > destW.size() && targetW[destW.size()] != L'\\' && targetW[destW.size()] != L'/')) {
            outError = "Security violation: detected Zip Slip path traversal in entry: " + rawName;
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
            outError = "Failed to decompress file: " + rawName;
            return false;
        }

        std::ofstream outFile(canonicalTarget, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            mz_free(pData);
            outError = "Failed to write target file: " + canonicalTarget.string();
            return false;
        }

        if (uncompSize > 0) {
            outFile.write(reinterpret_cast<const char*>(pData), uncompSize);
        }
        outFile.close();
        mz_free(pData);
    }

    return true;
}

} // anonymous namespace

bool extractZipArchive(const std::filesystem::path& zipPath,
                       const std::filesystem::path& destDir,
                       std::string& outError,
                       const ArchiveBombGuards& guards)
{
    std::error_code ec;
    if (!std::filesystem::exists(zipPath, ec) || !std::filesystem::is_regular_file(zipPath, ec)) {
        outError = "Archive file not found: " + zipPath.string();
        return false;
    }

    FILE* fp = _wfopen(zipPath.wstring().c_str(), L"rb");
    if (!fp) {
        outError = "Failed to open zip archive: " + zipPath.string();
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        std::fclose(fp);
        outError = "Corrupted or invalid ZIP archive.";
        return false;
    }

    bool success = extractZipArchiveCore(zip, destDir, outError, guards);

    mz_zip_reader_end(&zip);
    std::fclose(fp);
    return success;
}

bool extractZipArchive(const std::string& zipPath,
                       const std::string& destDir,
                       std::string& outError,
                       const ArchiveBombGuards& guards)
{
    return extractZipArchive(std::filesystem::path(zipPath), std::filesystem::path(destDir), outError, guards);
}

bool extractZipMemory(const void* data,
                      size_t dataSize,
                      const std::filesystem::path& destDir,
                      std::string& outError,
                      const ArchiveBombGuards& guards)
{
    if (!data || dataSize == 0) {
        outError = "Invalid memory buffer for ZIP extraction.";
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data, dataSize, 0)) {
        outError = "Corrupted or invalid ZIP archive in memory.";
        return false;
    }

    bool success = extractZipArchiveCore(zip, destDir, outError, guards);

    mz_zip_reader_end(&zip);
    return success;
}

bool ArchiveUtils::gzipDecompress(const std::vector<uint8_t>& gzData, std::string& outXml) {
    outXml.clear();
    if (gzData.size() < 18) return false;
    if (gzData[0] != 0x1F || gzData[1] != 0x8B || gzData[2] != 0x08) return false;

    uint8_t flg = gzData[3];
    size_t offset = 10;

    // FEXTRA
    if (flg & 0x04) {
        if (offset + 2 > gzData.size()) return false;
        uint16_t xlen = static_cast<uint16_t>(gzData[offset]) | (static_cast<uint16_t>(gzData[offset + 1]) << 8);
        offset += 2 + xlen;
    }
    // FNAME
    if (flg & 0x08) {
        while (offset < gzData.size() && gzData[offset] != 0) ++offset;
        ++offset;
    }
    // FCOMMENT
    if (flg & 0x10) {
        while (offset < gzData.size() && gzData[offset] != 0) ++offset;
        ++offset;
    }
    // FHCRC
    if (flg & 0x02) {
        offset += 2;
    }

    if (offset >= gzData.size() - 8) return false;

    size_t deflateLen = gzData.size() - 8 - offset;
    const uint8_t* deflateData = gzData.data() + offset;

    size_t uncompLen = 0;
    void* uncompBuf = tinfl_decompress_mem_to_heap(deflateData, deflateLen, &uncompLen, 0);
    if (!uncompBuf) return false;

    if (uncompLen > kMaxDecompressedXmlBytes) {
        mz_free(uncompBuf);
        return false;
    }

    if (deflateLen > 0 && (uncompLen / deflateLen) > kMaxCompressionRatio) {
        mz_free(uncompBuf);
        return false;
    }

    outXml.assign(reinterpret_cast<const char*>(uncompBuf), uncompLen);
    mz_free(uncompBuf);
    return true;
}

bool ArchiveUtils::gzipCompress(std::string_view xmlData, std::vector<uint8_t>& outGzData) {
    outGzData.clear();
    const mz_uint compFlags = tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -15, MZ_DEFAULT_STRATEGY);
    size_t compLen = 0;
    void* compBuf = tdefl_compress_mem_to_heap(xmlData.data(), xmlData.size(), &compLen, compFlags);
    if (!compBuf && !xmlData.empty()) return false;

    const uint8_t gzHeader[10] = {
        0x1F, 0x8B,
        0x08,
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00,
        0x0A
    };

    outGzData.insert(outGzData.end(), gzHeader, gzHeader + 10);
    if (compBuf && compLen > 0) {
        const uint8_t* compBytes = reinterpret_cast<const uint8_t*>(compBuf);
        outGzData.insert(outGzData.end(), compBytes, compBytes + compLen);
        mz_free(compBuf);
    }

    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(xmlData.data()), xmlData.size());
    uint32_t isize = static_cast<uint32_t>(xmlData.size());

    uint8_t trailer[8];
    trailer[0] = static_cast<uint8_t>(crc & 0xFF);
    trailer[1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    trailer[2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    trailer[3] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    trailer[4] = static_cast<uint8_t>(isize & 0xFF);
    trailer[5] = static_cast<uint8_t>((isize >> 8) & 0xFF);
    trailer[6] = static_cast<uint8_t>((isize >> 16) & 0xFF);
    trailer[7] = static_cast<uint8_t>((isize >> 24) & 0xFF);

    outGzData.insert(outGzData.end(), trailer, trailer + 8);
    return true;
}

bool ArchiveUtils::unzipFile(const std::filesystem::path& zipPath, const std::filesystem::path& destDir) {
    std::string err;
    ArchiveBombGuards guards;
    guards.maxSingleFileSize = kMaxDecompressedXmlBytes;
    guards.maxArchiveTotalSize = kMaxDecompressedTotalBytes;
    guards.maxFileCount = kMaxArchiveEntryCount;
    guards.maxCompressionRatio = kMaxCompressionRatio;
    return extractZipArchive(zipPath, destDir, err, guards);
}

bool ArchiveUtils::zipDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& zipPath) {
    std::error_code ec;
    if (!std::filesystem::exists(sourceDir, ec) || !std::filesystem::is_directory(sourceDir, ec)) {
        return false;
    }

    std::filesystem::remove(zipPath, ec);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

#if defined(_WIN32)
    FILE* fp = _wfopen(zipPath.wstring().c_str(), L"wb");
#else
    FILE* fp = std::fopen(zipPath.string().c_str(), "wb");
#endif
    if (!fp) return false;

    if (!mz_zip_writer_init_cfile(&zip, fp, 0)) {
        std::fclose(fp);
        return false;
    }

    bool ok = true;
    for (auto it = std::filesystem::recursive_directory_iterator(sourceDir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) { ok = false; break; }
        if (!it->is_regular_file()) continue;

        auto relPath = std::filesystem::relative(it->path(), sourceDir, ec);
        if (ec) { ok = false; break; }

        std::string relStr = relPath.generic_string();
        std::string filePathStr = it->path().string();

        if (!mz_zip_writer_add_file(&zip, relStr.c_str(), filePathStr.c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        if (!mz_zip_writer_finalize_archive(&zip)) {
            ok = false;
        }
    }

    mz_zip_writer_end(&zip);
    std::fclose(fp);
    return ok;
}

bool ArchiveUtils::sanitizeZipEntryPath(const std::string& entryName, std::filesystem::path& outSafePath) {
    return sanitizeZipPath(entryName, outSafePath);
}

} // namespace praccy::utils
