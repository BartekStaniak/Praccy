#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace praccy::utils {

/// Configurable decompression bomb thresholds per R6 specification
struct ArchiveBombGuards {
    uint64_t maxSingleFileSize   = 30ULL * 1024 * 1024;   // 30 MB (single file uncompressed limit)
    uint64_t maxXmlSize          = 30ULL * 1024 * 1024;   // 30 MB (decompression bomb guard for XML)
    uint64_t maxArchiveTotalSize = 128ULL * 1024 * 1024;  // 128 MB (total uncompressed archive limit)
    uint32_t maxCompressionRatio = 100;                   // 100:1 ratio guard (uncompressed / compressed)
    uint32_t maxFileCount        = 100;                   // 100 files max limit in archive
};

/// Win32 path sanitization for Zip Slip prevention
/// Validates and normalizes entryName, returning true and a sanitized relative path
/// if safe, or false if the entry violates any security constraints.
bool sanitizeZipPath(std::string_view entryName, std::filesystem::path& outSafeRelativePath);

/// In-process archive extraction wrapping vendored miniz
/// Checks all decompression bomb guards and path sanitization constraints before extracting
bool extractZipArchive(const std::filesystem::path& zipPath,
                       const std::filesystem::path& destDir,
                       std::string& outError,
                       const ArchiveBombGuards& guards = ArchiveBombGuards{});

/// std::string overload for string path inputs
bool extractZipArchive(const std::string& zipPath,
                       const std::string& destDir,
                       std::string& outError,
                       const ArchiveBombGuards& guards = ArchiveBombGuards{});

/// In-memory archive extraction wrapping vendored miniz
bool extractZipMemory(const void* data,
                      size_t dataSize,
                      const std::filesystem::path& destDir,
                      std::string& outError,
                      const ArchiveBombGuards& guards = ArchiveBombGuards{});

class ArchiveUtils {
public:
    // In-process Gzip (Ableton Live .adg)
    static bool gzipDecompress(const std::vector<uint8_t>& gzData, std::string& outXml);
    static bool gzipCompress(std::string_view xmlData, std::vector<uint8_t>& outGzData);

    // In-process ZIP (DAWproject .dawproject & Updates)
    static bool unzipFile(const std::filesystem::path& zipPath, const std::filesystem::path& destDir);
    static bool zipDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& zipPath);

    // SecOps path traversal & decompression sanitization
    static bool sanitizeZipEntryPath(const std::string& entryName, std::filesystem::path& outSafePath);

    // Bounds limits (differentiated for XML vs multi-file audio archives)
    static constexpr size_t kMaxDecompressedXmlBytes   = 30 * 1024 * 1024;  // 30 MB (XML DOM limit)
    static constexpr size_t kMaxDecompressedTotalBytes = 128 * 1024 * 1024; // 128 MB (Audio IRs/Assets)
    static constexpr size_t kMaxArchiveEntryCount      = 100;               // 100 files
    static constexpr size_t kMaxCompressionRatio       = 100;               // 100:1 bomb limit
};

} // namespace praccy::utils
