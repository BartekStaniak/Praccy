# Implementation Blueprint: Features 7 & 8 — In-Process Archive Extraction & Direct Updater Execution

**Author**: Explorer Subagent (`teamwork_preview_explorer_m2_1`)  
**Milestone**: Milestone 2 (SecOps, Crash Isolation & Hardening)  
**Date**: 2026-10-06T20:26:00Z  
**Target Scope**: Feature 7 (In-Process ZIP Extraction with `miniz` and Strict Zip Slip Defense) & Feature 8 (Direct Updater Execution without `cmd.exe` or `powershell.exe`)  
**Working Directory**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/`  

---

## 1. Observation

### 1.1 Update Archive Extraction Analysis (`src/ui/update_checker.cpp`)
Inspection of `src/ui/update_checker.cpp` reveals that archive extraction currently delegates directly to external shell processes via `std::system()`:
```cpp
// Lines 329-338:
// 1. Try Windows built-in tar
std::string tarCmd = "tar -xf \"" + zipPath.string() + "\" -C \"" + extractDir.string() + "\"";
int tarRet = std::system(tarCmd.c_str());

// 2. If tar failed, fall back to PowerShell Expand-Archive
if (tarRet != 0) {
    std::string psCmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path '" +
        zipPath.string() + "' -DestinationPath '" + extractDir.string() + "' -Force\"";
    std::system(psCmd.c_str());
}
```

**Vulnerabilities & Shortcomings**:
1. Spawning `tar` or `powershell` invokes `%COMSPEC%` (`cmd.exe`), causing console window flashes and potential thread hangs.
2. In enterprise or restricted environments where PowerShell or tar execution is blocked by AppLocker / Software Restriction Policies, the auto-updater fails completely.
3. String interpolation (`"tar -xf \"" + zipPath.string() + ...`) is vulnerable to argument and command injection if file paths contain shell meta-characters (`&`, `|`, `;`, `"`, `%`).
4. Acceptance Criteria explicitly demands:
   > *"Static analysis string inspection confirms zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe` in binary/updater code."*

### 1.2 Binary Updater & Restart Analysis (`src/ui/update_checker.cpp` & `src/main.cpp`)
Inspection of `applyUpdateAndRestart()` in `src/ui/update_checker.cpp` reveals reliance on on-the-fly batch script generation and `cmd.exe` process execution:
```cpp
// Lines 382-419:
auto updateDir = std::filesystem::path(state::AppConfig::getConfigDir()) / "updates";
auto batPath = updateDir / "apply_update.bat";

// Write updater batch script
std::ofstream bat(batPath);
if (!bat.is_open()) return false;

bat << "@echo off\n";
bat << "setlocal\n";
bat << "set \"PID=%~1\"\n";
bat << "set \"SRC=%~2\"\n";
bat << "set \"DST=%~3\"\n";
bat << "set \"SRCDIR=%~dp2\"\n";
bat << "set \"DSTDIR=%~dp3\"\n";
bat << "\n";
bat << "timeout /t 1 /nobreak >nul\n";
bat << "\n";
bat << ":wait_loop\n";
bat << "tasklist /fi \"PID eq %PID%\" 2>nul | find \"%PID%\" >nul\n";
bat << "if %errorlevel% equ 0 (\n";
bat << "    timeout /t 1 /nobreak >nul\n";
bat << "    goto wait_loop\n";
bat << ")\n";
bat << "\n";
bat << "copy /y \"%SRC%\" \"%DST%\" >nul\n";
bat << "if exist \"%SRCDIR%resources\" (\n";
bat << "    xcopy /e /i /y \"%SRCDIR%resources\" \"%DSTDIR%resources\" >nul\n";
bat << ")\n";
bat << "\n";
bat << "start \"\" \"%DST%\"\n";
bat << "exit\n";
bat.close();

// Prepare CreateProcess command
std::wstring wBat = batPath.wstring();
std::wstring wNew = std::filesystem::path(newExePath).wstring();
std::wstring wCur = currentExe;

std::wstring cmdLine = L"cmd.exe /c \"" + wBat + L"\" " + std::to_wstring(currentPid) + L" \"" + wNew + L"\" \"" + wCur + L"\"";

STARTUPINFOW si{};
si.cb = sizeof(si);
si.dwFlags = STARTF_USESHOWWINDOW;
si.wShowWindow = SW_HIDE;

PROCESS_INFORMATION pi{};
BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
```

**Vulnerabilities & Shortcomings**:
1. Invokes `cmd.exe /c` directly, introducing an explicit acceptance criteria violation.
2. Generates an executable `.bat` script file in the user's roaming AppData directory, which can trigger Endpoint Detection and Response (EDR) / antivirus false positives.
3. Polling process existence with `tasklist` in a 1-second timeout loop is inefficient and can fail if the process terminates asynchronously between polling intervals.
4. Invokes external `xcopy` to synchronize the `resources/` folder.
5. In `src/main.cpp`, entry point `WinMain(HINSTANCE, HINSTANCE, LPSTR, int)` currently does not inspect command-line arguments, meaning no updater sub-command mode exists yet.

### 1.3 Codebase-Wide Audit for Prohibited Shell & Process Invocations
Comprehensive static searches across the repository yielded:
- **`std::system`**: Exactly 2 occurrences, both in `src/ui/update_checker.cpp` (lines 331 and 337). No occurrences elsewhere in `src/`, `tests/`, or headers.
- **`cmd.exe`**: Exactly 1 occurrence in `src/ui/update_checker.cpp` (line 419). Zero occurrences elsewhere.
- **`powershell.exe` / `powershell`**: Occurrences only in `src/ui/update_checker.cpp` (lines 333, 335) and markdown documentation (`README.md`).
- **`apply_update.bat`**: Occurs only in `src/ui/update_checker.cpp:382`.
- **`CreateProcessW`**: Occurs only in `src/ui/update_checker.cpp:427`.
- **`ShellExecuteA`**: Used solely in `src/ui/rack_view.cpp` (lines 2738, 2789, 2967) to open GitHub URLs in the default user web browser (`"https://github.com/praccy/praccy"`).

### 1.4 Build System & Linker Inventory (`CMakeLists.txt`)
Inspection of `CMakeLists.txt`:
1. Line 10 specifies: `project(Praccy VERSION 1.1.3 LANGUAGES CXX C RC)`. C language compilation is already enabled.
2. `PRACCY_SOURCES` lists all first-party and third-party files.
3. `target_include_directories(Praccy PRIVATE ...)` includes `src`, `resources`, and `third_party`.
4. `target_link_libraries(Praccy PRIVATE ...)` links `kernel32`, `user32`, `wininet`, etc., but does **not** currently link `shell32`.
   - Parsing Unicode command-line arguments via `CommandLineToArgvW` requires `shell32` (`#include <shellapi.h>`).

---

## 2. Logic Chain

### 2.1 From Shell Extraction to In-Process Extraction with `miniz`
1. External invocations of `tar` and `powershell.exe` violate the core requirement of zero external shell calls and introduce security/portability failure modes (*Observation 1.1*).
2. `miniz` is an established, public-domain, zero-dependency, amalgamated ANSI C ZIP compression and decompression library consisting of a single header (`miniz.h`) and single source file (`miniz.c`).
3. Adding `third_party/miniz/miniz.h` and `third_party/miniz/miniz.c` provides in-process archive reading via `mz_zip_reader_*` APIs.
4. Because `miniz.c` is third-party C code, compiling under strict `/W4 /WX` (MSVC) or `-Wall -Wextra -Werror` (GCC) must isolate warnings on `miniz.c` (e.g., via `set_source_files_properties` or CMake `SYSTEM` include directories) to prevent third-party warnings from breaking the build.
5. Windows file paths can contain non-ASCII Unicode characters. Opening the archive via `_wfopen(zipPath.wstring().c_str(), L"rb")` and initializing miniz via `mz_zip_reader_init_cfile` guarantees Unicode path compatibility on Windows without codepage issues.

### 2.2 Strict Zip Slip & Decompression Bomb Defenses
1. ZIP archives store arbitrary entry paths provided by archive creators. An attacker could craft an archive containing entries like `../../Windows/System32/malicious.dll` or `foo/../../bar`.
2. Standard extraction without sanitization extracts files relative to the target directory by simple string concatenation, leading to Zip Slip directory traversal (CWE-22).
3. To achieve bulletproof Zip Slip protection, an entry path sanitizer `sanitizeZipEntryPath` must enforce multi-layer validation before any file creation:
   - **Layer 1: Path Separator Normalization**: Convert all backslashes (`\`) to forward slashes (`/`).
   - **Layer 2: Absolute & Drive Rejection**: Reject any path starting with `/`, `\`, drive letters (`C:`), or UNC prefixes (`//`, `\\`).
   - **Layer 3: Token Validation**: Tokenize by `/`. Reject any token matching `.` or `..`. Reject tokens containing Windows invalid filename characters (`<`, `>`, `:`, `"`, `|`, `?`, `*`) or ASCII control codes (< 32). Reject tokens ending with trailing dots or spaces.
   - **Layer 4: Reserved Device Name Protection**: Reject tokens matching DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-COM9`, `LPT1-LPT9`), whether with or without an extension.
   - **Layer 5: Canonical Destination Prefix Containment**: Compute `canonicalTarget = std::filesystem::weakly_canonical(destDir / sanitizedRelPath)`. Verify that `canonicalTarget` is strictly prefixed by `canonicalDest = std::filesystem::weakly_canonical(destDir)`.
4. To defend against decompression bombs (Zip Bombs, CWE-409):
   - Reject any single file whose uncompressed size exceeds 250 MB (`MAX_SINGLE_FILE_SIZE`).
   - Reject any archive whose cumulative uncompressed size exceeds 500 MB (`MAX_TOTAL_UNCOMPRESSED_SIZE`).
   - Reject any archive containing more than 10,000 entries (`MAX_FILE_COUNT`).
   - Reject suspicious compression ratios exceeding 100:1 for files > 10 MB.

### 2.3 From `apply_update.bat` to Direct Updater Execution (`Praccy.exe --apply-update`)
1. Spawning `cmd.exe /c apply_update.bat` violates Acceptance Criteria and generates unnecessary scripts on disk (*Observation 1.2*).
2. The downloaded and extracted executable is an identical or newer build of `Praccy.exe`.
3. When the user clicks "Restart & Apply Update Now":
   - The running instance (`Instance A`) obtains its current PID (`GetCurrentProcessId()`) and current module path (`GetModuleFileNameW`).
   - `Instance A` executes the extracted binary (`Instance B`) directly using Win32 `CreateProcessW`:
     `"<extractedPath>\Praccy.exe" --apply-update <currentPid> "<currentExePath>"`
   - `Instance A` terminates cleanly via `ExitProcess(0)`.
4. In `src/main.cpp`, before initializing DirectX, ImGui, or audio devices, `WinMain` checks `CommandLineToArgvW`:
   - If argument 1 is `--apply-update`, it invokes `runDirectUpdater(oldPid, destExePath)`:
     a. **Synchronize & Wait**: Opens `Instance A`'s process handle via `OpenProcess(SYNCHRONIZE, FALSE, oldPid)`. If valid, waits up to 15,000 ms via `WaitForSingleObject(hProcess, 15000)`. Once `Instance A` exits, the file lock on `currentExePath` is released.
     b. **Binary Replacement with Retry**: Identifies its own path via `GetModuleFileNameW`. Executes a retry loop (up to 20 attempts with 100ms `Sleep`) calling Win32 `CopyFileW(selfPath, destExePath.c_str(), FALSE)`. This accommodates minor delays while the OS releases file handles or antivirus scanners complete.
     c. **Resource Synchronization**: Uses C++20 `std::filesystem::copy` with `copy_options::recursive | copy_options::overwrite_existing` to update `resources/` in-process, eliminating `xcopy`.
     d. **Seamless Relaunch**: Calls Win32 `CreateProcessW` to relaunch `destExePath` without updater arguments.
     e. **Exit**: `Instance B` exits cleanly via `ExitProcess(0)`.
5. This completely eliminates `.bat` files, `cmd.exe`, `powershell.exe`, and `std::system`.

---

## 3. Caveats

1. **File Overwrite Permissions**:
   - If Praccy is installed into a protected system directory (e.g., `C:\Program Files\Praccy\`) and run without administrator privileges, `CopyFileW` will fail with `ERROR_ACCESS_DENIED`.
   - The updater design must check for this condition and display an informative Win32 `MessageBoxW` error dialog rather than failing silently.
2. **Self-Deletion on Windows**:
   - Windows does not allow a running `.exe` to delete itself. Therefore, `Instance B` (the extracted binary in `updates/extracted/`) cannot delete itself before exiting.
   - However, once `destExe` relaunches, Praccy's subsequent update checks or downloads can safely clean up older files inside `getConfigDir() / "updates" / "extracted"`.
3. **Compiler and Warning Flags**:
   - `miniz.c` is written in standard C99. When compiling with GCC `-Wall -Wextra -Werror` or MSVC `/W4 /WX`, third-party warnings in `miniz.c` must be suppressed using CMake `COMPILE_OPTIONS` / `COMPILE_FLAGS` property for that file, or by adding `third_party/miniz` as `SYSTEM` include directory.
4. **Shell32 Linkage**:
   - `CommandLineToArgvW` resides in `shell32.dll`. `CMakeLists.txt` must explicitly include `shell32` in `target_link_libraries`.

---

## 4. Conclusion & Concrete Blueprint

### 4.1 File Inventory for Features 7 & 8
| File Path | Action | Description |
|---|---|---|
| `third_party/miniz/miniz.h` | **New** | Standard single-header C lossless compression & ZIP archive reader/writer header |
| `third_party/miniz/miniz.c` | **New** | Standard single-source C implementation of miniz |
| `CMakeLists.txt` | **Modify** | Add `third_party/miniz/miniz.c` to sources, add `shell32` to link libraries, configure warning suppression for `miniz.c` |
| `src/ui/update_checker.h` | **Modify** | Expose `sanitizeZipEntryPath` and `extractZipArchive` declarations for in-process extraction and testing |
| `src/ui/update_checker.cpp` | **Modify** | Implement `extractZipArchive` using miniz with strict Zip Slip sanitization; replace `applyUpdateAndRestart` with direct `CreateProcessW` calling `--apply-update` |
| `src/main.cpp` | **Modify** | Intercept `--apply-update <pid> "<dest>"` at top of `WinMain`; implement `runDirectUpdater` using `OpenProcess`, `WaitForSingleObject`, and `CopyFileW` |
| `tests/test_praccy.cpp` | **Modify** | Add unit tests for Zip Slip path sanitization, in-process miniz archive extraction, and zero-prohibited-strings static analysis assertion |

---

### 4.2 Vendoring `miniz` (`third_party/miniz/`)

1. Author/Place `third_party/miniz/miniz.h` and `third_party/miniz/miniz.c`.
2. Public API capabilities utilized:
   - `mz_zip_archive`
   - `mz_zip_reader_init_cfile`
   - `mz_zip_reader_get_num_files`
   - `mz_zip_reader_file_stat`
   - `mz_zip_reader_is_file_a_directory`
   - `mz_zip_reader_extract_to_heap`
   - `mz_zip_reader_end`
   - `mz_free`
   - `mz_zip_writer_*` (for generating test archives in unit tests)

---

### 4.3 CMakeLists.txt Changes

```cmake
# In CMakeLists.txt:
# 1. Add third_party/miniz/miniz.c to PRACCY_SOURCES:
set(PRACCY_SOURCES
    ...
    third_party/miniz/miniz.c
    ...
)

# 2. Add include directory and suppress warnings on third_party/miniz/miniz.c:
target_include_directories(Praccy PRIVATE
    ...
    third_party/miniz
)

if(MSVC)
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_FLAGS "/W3")
else()
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_FLAGS "-w")
endif()

# 3. Add shell32 to target_link_libraries:
target_link_libraries(Praccy PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    winmm
    avrt
    ole32
    uuid
    gdi32
    gdiplus
    wininet
    user32
    kernel32
    shell32   # <-- For CommandLineToArgvW
)

# 4. Add miniz.c and shell32 to test_praccy target:
# (Allows test_praccy to execute in-process ZIP extraction & Zip Slip unit tests)
add_executable(test_praccy
    tests/test_praccy.cpp
    src/ui/update_checker.cpp   # or extracted utils
    third_party/miniz/miniz.c
    ...
)
target_include_directories(test_praccy PRIVATE
    src
    third_party
    third_party/miniz
    third_party/clap/include
)
target_link_libraries(test_praccy PRIVATE ole32 uuid user32 gdi32 winmm avrt wininet shell32)
```

---

### 4.4 In-Process ZIP Extraction & Strict Zip Slip Defense (`src/ui/update_checker.cpp`)

#### 4.4.1 Strict Zip Slip Sanitizer Implementation
```cpp
#include "miniz.h"
#include <vector>
#include <sstream>
#include <algorithm>

namespace praccy::ui {

/**
 * Validates and sanitizes a ZIP archive entry path to strictly prevent
 * Zip Slip directory traversal (CWE-22) and Windows reserved name exploitation.
 *
 * @param entryName The raw filename stored in the ZIP archive central directory.
 * @param outSafeRelativePath Populated with the safe relative path if valid.
 * @return True if safe; false if directory traversal or invalid characters detected.
 */
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
```

#### 4.4.2 In-Process `extractZipArchive` Implementation
```cpp
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

} // namespace praccy::ui
```

#### 4.4.3 Call Site Replacement in `UpdateChecker::runDownload()`
In `src/ui/update_checker.cpp`, replace lines 329-338 (`tarCmd`, `std::system`, `psCmd`) with:
```cpp
    // In-process archive extraction replacing tar and powershell.exe
    std::string extractError;
    if (!extractZipArchive(zipPath, extractDir, extractError)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_info.status = UpdateStatus::Error;
        m_info.errorMessage = "Failed to extract update package: " + extractError;
        return;
    }
```

---

### 4.5 Direct Updater Restart Blueprint (`src/ui/update_checker.cpp` & `src/main.cpp`)

#### 4.5.1 Caller Side: Direct Launch from `UpdateChecker::applyUpdateAndRestart()`
Replace lines 384-434 in `src/ui/update_checker.cpp` with:
```cpp
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
```

#### 4.5.2 Relauncher Side: `runDirectUpdater` in `src/main.cpp`
Add the direct updater handler in `src/main.cpp` before `WinMain`:
```cpp
#include <shellapi.h>
#include <filesystem>

static int runDirectUpdater(DWORD oldPid, const std::wstring& destExe) {
    // 1. Wait for parent Praccy process to terminate and release file lock
    if (oldPid > 0) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
        if (hProcess != nullptr) {
            WaitForSingleObject(hProcess, 15000); // 15-second timeout
            CloseHandle(hProcess);
        }
    }

    // 2. Identify updater binary path (self)
    wchar_t selfPath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0) {
        return 1;
    }

    // 3. Robust retry loop for binary copy (handles lingering OS/AV locks)
    bool copySuccess = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (CopyFileW(selfPath, destExe.c_str(), FALSE)) {
            copySuccess = true;
            break;
        }
        Sleep(100);
    }

    if (!copySuccess) {
        DWORD err = GetLastError();
        std::wstring msg = L"Failed to overwrite Praccy executable (Error: " + std::to_wstring(err) + L").";
        if (err == ERROR_ACCESS_DENIED) {
            msg += L"\nPlease run the updater with administrator privileges.";
        }
        MessageBoxW(nullptr, msg.c_str(), L"Praccy Update Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // 4. Synchronize resources folder if bundled in extracted package
    std::error_code ec;
    std::filesystem::path selfDir = std::filesystem::path(selfPath).parent_path();
    std::filesystem::path destDir = std::filesystem::path(destExe).parent_path();
    std::filesystem::path srcRes = selfDir / "resources";
    std::filesystem::path dstRes = destDir / "resources";
    if (std::filesystem::exists(srcRes, ec)) {
        std::filesystem::copy(srcRes, dstRes,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }

    // 5. Relaunch newly installed Praccy executable at permanent destination
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring relaunchCmd = L"\"" + destExe + L"\"";

    BOOL ok = CreateProcessW(
        destExe.c_str(),
        relaunchCmd.data(),
        nullptr, nullptr, FALSE,
        0,
        nullptr, nullptr, &si, &pi
    );

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return 0;
}
```

At the very top of `WinMain` in `src/main.cpp`:
```cpp
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Check for direct updater execution flag before initializing GUI or audio
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 4 && wcscmp(argv[1], L"--apply-update") == 0) {
        DWORD oldPid = static_cast<DWORD>(wcstoul(argv[2], nullptr, 10));
        std::wstring destExe = argv[3];
        LocalFree(argv);
        return runDirectUpdater(oldPid, destExe);
    }
    if (argv) {
        LocalFree(argv);
    }

    // Normal startup continues...
    SetProcessDPIAware();
    ...
```

---

### 4.6 Unit Test Specification (`tests/test_praccy.cpp`)

Add the following 3 dedicated tests to `tests/test_praccy.cpp`:

1. **`testZipSlipSanitization()`**:
   - Verify valid paths: `"Praccy.exe"`, `"plugins/fx.dll"`, `"nested/sub/data.bin"`.
   - Verify directory traversal attacks are rejected:
     - `"../evil.exe"`
     - `"../../windows/system32/calc.exe"`
     - `"foo/../../bar.exe"`
     - `"/absolute/unix/path"`
     - `"\\absolute\\windows\\path"`
     - `"C:\\windows\\system32\\cmd.exe"`
     - `"C:foo.exe"`
     - `"\\\\unc\\share\\file.exe"`
     - `"CON.txt"`, `"aux.dll"`, `"NUL"`
     - `"file:stream.exe"`
     - `"trailing_space.txt "`
     - `"trailing_dot.txt."`
   - Assert all attack vectors return `false`.

2. **`testInProcessMinizArchiveExtraction()`**:
   - Programmatically create a valid test ZIP archive using miniz writer functions (`mz_zip_writer_init_file`, `mz_zip_writer_add_mem`).
   - Extract the test archive using `extractZipArchive()`.
   - Assert all extracted files match expected content and size.
   - Programmatically create an archive with a Zip Slip entry (`"../escaped.txt"`).
   - Assert `extractZipArchive()` rejects the archive with a security error message and does not write outside the destination directory.

3. **`testZeroProhibitedCommandsInCodebase()`**:
   - Inspect source files in `src/` to verify zero occurrences of:
     - `std::system`
     - `cmd.exe`
     - `powershell.exe`
     - `tar -xf`
     - `apply_update.bat`

---

## 5. Verification Method

### 5.1 Static Analysis Inspection Commands
Run ripgrep / PowerShell commands to verify zero prohibited strings across the codebase:
```powershell
Get-ChildItem -Path f:\Projects\Praccy\src -Recurse -Include *.cpp,*.h | Select-String -Pattern "std::system\(", "cmd\.exe", "powershell\.exe", "tar -xf", "apply_update\.bat"
```
*Expected Result*: Exactly 0 matches found.

### 5.2 Build Verification
Verify clean build under GCC (or MSVC) with zero warnings or errors:
```powershell
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build clean
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build test_praccy
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build Praccy
```
*Expected Result*: Exit code 0, cleanly built executables.

### 5.3 Automated Test Suite Execution
Execute the unit test harness:
```powershell
f:\Projects\Praccy\build\test_praccy.exe
```
*Expected Result*: All tests pass (including Zip Slip sanitization, in-process extraction, and prohibited string assertions).

### 5.4 Invalidation Conditions
This architectural blueprint is invalidated if:
1. Extracting a ZIP archive containing `../` writes any file outside the designated extraction directory.
2. An update leaves or creates any `.bat` file on disk.
3. The binary or updater code invokes `cmd.exe`, `powershell.exe`, or `std::system`.
4. Relaunching the updated executable hangs waiting for the parent PID if the parent PID already exited prior to `OpenProcess`.
