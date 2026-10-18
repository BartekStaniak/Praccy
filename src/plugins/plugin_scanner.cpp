#include "plugin_scanner.h"
#include "crash_isolation.h"
#include "../state/app_config.h"
#include "../utils/guid_utils.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>
#include <unordered_set>
#if defined(_WIN32)
#include <windows.h>
#else
#include <strings.h>
#define _stricmp strcasecmp
using HMODULE = void*;
#endif
#include <pluginterfaces/base/ipluginbase.h>
#include <clap/clap.h>

namespace praccy::plugins {

using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API *)();

namespace {

std::string toLowerString(std::string_view str) {
    std::string out(str);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string cleanName(std::string_view rawName) {
    std::string name(rawName);
    if (name.size() >= 5) {
        std::string ext = name.substr(name.size() - 5);
        std::string extLower = toLowerString(ext);
        if (extLower == ".vst3" || extLower == ".clap") {
            name.resize(name.size() - 5);
        }
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

bool matchesFavoriteString(const std::string& fav, const std::string& target) {
    if (fav.empty() || target.empty()) return false;
    if (fav == target) return true;

    std::filesystem::path pFav(fav);
    std::filesystem::path pTarget(target);
    std::string favStem = pFav.stem().string();
    std::string targetStem = pTarget.stem().string();

    if (favStem == target || fav == targetStem || favStem == targetStem) return true;

    std::string cleanFav = toLowerString(cleanName(favStem));
    std::string cleanTarget = toLowerString(cleanName(targetStem));
    return !cleanFav.empty() && cleanFav == cleanTarget;
}

} // namespace

PluginScanner::PluginScanner(const std::string& configPath)
    : m_configPath(configPath)
{
    addDefaultPaths();

    // Register built-in effects immediately so they are available right away
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Drive",
        .path = "builtin://drive",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Distortion",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Amp Sim",
        .path = "builtin://amp",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Amp Emulation",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Stereo Delay",
        .path = "builtin://delay",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Delay/Echo",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });

    // Load favorites, custom categories, custom paths, and cached scanned plugins
    state::AppConfig cfg;
    if (cfg.load(m_configPath)) {
        loadFromConfig(cfg);
    }
}

PluginScanner::~PluginScanner() {
    m_shouldStop = true;
    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
}

std::vector<std::string> PluginScanner::searchPaths() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_searchPaths;
}

std::vector<PluginDescriptor> PluginScanner::scannedPlugins() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_plugins;
}

size_t PluginScanner::numPlugins() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_plugins.size();
}

void PluginScanner::addPlugin(const PluginDescriptor& desc) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& d) {
        return d.name == desc.name && d.type == desc.type;
    });
    if (it == m_plugins.end()) {
        if (m_plugins.size() == m_plugins.capacity()) {
            std::vector<PluginDescriptor> updated = m_plugins;
            updated.push_back(desc);
            { if (m_retiredPluginSnapshots.size() >= 2) m_retiredPluginSnapshots.erase(m_retiredPluginSnapshots.begin()); m_retiredPluginSnapshots.push_back(std::move(m_plugins)); }
            m_plugins = std::move(updated);
        } else {
            m_plugins.push_back(desc);
        }
    } else {
        *it = desc;
    }
}

void PluginScanner::loadFromConfig(const state::AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_favorites.clear();
    for (const auto& fav : cfg.favoritePlugins) {
        if (!fav.empty() && std::find(m_favorites.begin(), m_favorites.end(), fav) == m_favorites.end()) {
            m_favorites.push_back(fav);
        }
    }
    m_favoritesInitialized = true;
    for (const auto& [name, cat] : cfg.customPluginCategories) {
        m_categoryOverrides[name] = cat;
    }
    for (const auto& cp : cfg.customPluginPaths) {
        if (!cp.empty() && std::find(m_searchPaths.begin(), m_searchPaths.end(), cp) == m_searchPaths.end()) {
            m_searchPaths.push_back(cp);
        }
    }
    for (const auto& sp : cfg.scannedPlugins) {
        PluginDescriptor desc;
        desc.name = sp.name;
        desc.path = sp.path;
        if (sp.type == "CLAP" || sp.type == "clap" || sp.type == "1") {
            desc.type = PluginType::CLAP;
        } else if (sp.type == "VST3" || sp.type == "vst3" || sp.type == "2") {
            desc.type = PluginType::VST3;
        } else {
            desc.type = PluginType::BuiltIn;
        }
        desc.vendor = sp.vendor;
        auto catIt = m_categoryOverrides.find(sp.name);
        if (catIt != m_categoryOverrides.end()) {
            desc.category = catIt->second;
        } else {
            desc.category = detectPluginCategory(sp.name, sp.vendor);
        }
        desc.bundleFileName = sp.bundleFileName;
        desc.classGuid = sp.classGuid;
        desc.clapId = sp.clapId;

        auto it = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& existing) {
            return existing.name == desc.name;
        });
        if (it == m_plugins.end()) {
            if (m_plugins.size() == m_plugins.capacity()) {
                std::vector<PluginDescriptor> updated = m_plugins;
                updated.push_back(std::move(desc));
                { if (m_retiredPluginSnapshots.size() >= 2) m_retiredPluginSnapshots.erase(m_retiredPluginSnapshots.begin()); m_retiredPluginSnapshots.push_back(std::move(m_plugins)); }
                m_plugins = std::move(updated);
            } else {
                m_plugins.push_back(std::move(desc));
            }
        } else {
            it->type = desc.type;
            it->category = desc.category;
            it->vendor = desc.vendor;
            if (!desc.bundleFileName.empty()) it->bundleFileName = desc.bundleFileName;
            if (!desc.classGuid.empty()) it->classGuid = desc.classGuid;
            if (!desc.clapId.empty()) it->clapId = desc.clapId;
        }
    }
    for (auto& p : m_plugins) {
        auto catIt = m_categoryOverrides.find(p.name);
        if (catIt != m_categoryOverrides.end()) {
            p.category = catIt->second;
        }
    }
}

void PluginScanner::exportToConfig(state::AppConfig& cfg) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    cfg.scannedPlugins.clear();
    for (const auto& p : m_plugins) {
        if (p.type == PluginType::BuiltIn || p.path.starts_with("builtin://")) {
            continue;
        }
        state::ScannedPluginEntry entry;
        entry.name = p.name;
        entry.path = p.path;
        entry.type = p.typeString();
        entry.vendor = p.vendor;
        entry.category = p.category;
        entry.bundleFileName = p.bundleFileName;
        entry.classGuid = p.classGuid;
        entry.clapId = p.clapId;
        cfg.scannedPlugins.push_back(std::move(entry));
    }
    if (m_favoritesInitialized) {
        cfg.favoritePlugins = m_favorites;
    } else if (!m_favorites.empty()) {
        cfg.favoritePlugins = m_favorites;
    }
    cfg.customPluginCategories = m_categoryOverrides;
}

void PluginScanner::saveToAppConfig(const std::string& customPath) const {
    std::string target = customPath.empty() ? m_configPath : customPath;
    state::AppConfig cfg;
    cfg.load(target);
    exportToConfig(cfg);
    cfg.save(target);
}

void PluginScanner::loadFromAppConfig(const std::string& customPath) {
    std::string target = customPath.empty() ? m_configPath : customPath;
    state::AppConfig cfg;
    if (cfg.load(target)) {
        loadFromConfig(cfg);
    }
}

void PluginScanner::addDefaultPaths() {
    m_searchPaths.clear();

    // Standard 64-bit VST3 & CLAP directories on Windows
    m_searchPaths.push_back("C:\\Program Files\\Common Files\\VST3");
    m_searchPaths.push_back("C:\\Program Files\\Common Files\\CLAP");
    m_searchPaths.push_back("C:\\Program Files\\VstPlugins");
    m_searchPaths.push_back("C:\\Program Files\\Steinberg\\VstPlugins");

    // User LocalAppData locations
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp) {
        std::string lap(localApp);
        m_searchPaths.push_back(lap + "\\Programs\\Common\\VST3");
        m_searchPaths.push_back(lap + "\\Programs\\Common\\CLAP");
    }
}

bool PluginScanner::isSystemDefaultPath(const std::string& path) const {
    static const std::vector<std::string> defaultRoots = []() {
        std::vector<std::string> defaults = {
            "C:\\Program Files\\Common Files\\VST3",
            "C:\\Program Files\\Common Files\\CLAP",
            "C:\\Program Files\\VstPlugins",
            "C:\\Program Files\\Steinberg\\VstPlugins"
        };
        const char* localApp = std::getenv("LOCALAPPDATA");
        if (localApp) {
            std::string lap(localApp);
            defaults.push_back(lap + "\\Programs\\Common\\VST3");
            defaults.push_back(lap + "\\Programs\\Common\\CLAP");
        }
        return defaults;
    }();

    for (const auto& def : defaultRoots) {
        if (_stricmp(path.c_str(), def.c_str()) == 0) return true;
    }
    return false;
}

void PluginScanner::addCustomSearchPath(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_searchPaths.begin(), m_searchPaths.end(), path) == m_searchPaths.end()) {
        m_searchPaths.push_back(path);
    }
}

void PluginScanner::removeCustomSearchPath(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_searchPaths.size()) {
        m_searchPaths.erase(m_searchPaths.begin() + index);
    }
}

std::string PluginScanner::detectVendor(const std::filesystem::path& path, const std::string& name) {
    std::string pathStr = path.string();
    std::string nameLower = name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) { return std::tolower(c); });

    // 1. Signature heuristics for major plugin ecosystems
    if (nameLower.find("archetype") != std::string::npos ||
        nameLower.find("nolly") != std::string::npos ||
        nameLower.find("plini") != std::string::npos ||
        nameLower.find("soldano") != std::string::npos ||
        nameLower.find("fortin") != std::string::npos ||
        nameLower.find("cory wong") != std::string::npos ||
        nameLower.find("gojira") != std::string::npos ||
        nameLower.find("tim henson") != std::string::npos ||
        nameLower.find("petrucci") != std::string::npos ||
        nameLower.find("rabea") != std::string::npos ||
        nameLower.find("parallax") != std::string::npos ||
        nameLower.find("darkglass") != std::string::npos ||
        nameLower.find("mateus asato") != std::string::npos ||
        pathStr.find("Neural DSP") != std::string::npos) {
        return "Neural DSP";
    }

    if (nameLower.find("fabfilter") != std::string::npos ||
        nameLower.find("pro-q") != std::string::npos ||
        nameLower.find("pro-c") != std::string::npos ||
        nameLower.find("pro-l") != std::string::npos ||
        nameLower.find("pro-r") != std::string::npos ||
        nameLower.find("pro-mb") != std::string::npos ||
        nameLower.find("pro-ds") != std::string::npos ||
        nameLower.find("pro-g") != std::string::npos ||
        nameLower.find("saturn") != std::string::npos ||
        nameLower.find("timeless") != std::string::npos ||
        nameLower.find("volcano") != std::string::npos ||
        pathStr.find("FabFilter") != std::string::npos) {
        return "FabFilter";
    }

    if (nameLower.find("izotope") != std::string::npos ||
        nameLower.find("ozone") != std::string::npos ||
        nameLower.find("neutron") != std::string::npos ||
        nameLower.find("nectar") != std::string::npos ||
        nameLower.find("trash") != std::string::npos ||
        nameLower.find("vinyl") != std::string::npos ||
        nameLower.find("rx ") != std::string::npos ||
        nameLower.find("ddly") != std::string::npos ||
        nameLower.find("vocal doubler") != std::string::npos ||
        nameLower.find("stutter edit") != std::string::npos ||
        pathStr.find("iZotope") != std::string::npos) {
        return "iZotope";
    }

    if (nameLower.find("kontakt") != std::string::npos ||
        nameLower.find("massive") != std::string::npos ||
        nameLower.find("raum") != std::string::npos ||
        nameLower.find("guitar rig") != std::string::npos ||
        pathStr.find("Native Instruments") != std::string::npos) {
        return "Native Instruments";
    }

    if (nameLower.find("soothe") != std::string::npos ||
        nameLower.find("spiff") != std::string::npos) {
        return "oeksound";
    }

    if (nameLower.find("pigments") != std::string::npos ||
        pathStr.find("Arturia") != std::string::npos) {
        return "Arturia";
    }

    if (nameLower.find("valhalla") != std::string::npos) {
        return "Valhalla DSP";
    }

    if (nameLower.find("splice") != std::string::npos ||
        nameLower.find("astra") != std::string::npos ||
        nameLower.find("beatmaker") != std::string::npos ||
        pathStr.find("Splice") != std::string::npos) {
        return "Splice";
    }

    if (nameLower.find("cradle") != std::string::npos ||
        nameLower.find("state machine") != std::string::npos ||
        pathStr.find("Cradle") != std::string::npos) {
        return "Cradle";
    }

    if (nameLower.find("lindell") != std::string::npos ||
        nameLower.find("maag") != std::string::npos ||
        nameLower.find("ltl ") != std::string::npos ||
        nameLower.find("spl ") != std::string::npos ||
        nameLower.find("unfiltered audio") != std::string::npos ||
        nameLower.find("shadow hills") != std::string::npos ||
        nameLower.find("adptr") != std::string::npos ||
        nameLower.find("metricab") != std::string::npos ||
        nameLower.find("acme") != std::string::npos ||
        nameLower.find("opticom") != std::string::npos ||
        nameLower.find("ampeg") != std::string::npos ||
        nameLower.find("brainworx") != std::string::npos ||
        nameLower.find("bx_") != std::string::npos) {
        return "Plugin Alliance";
    }

    if (nameLower.find("beam") != std::string::npos ||
        nameLower.find("lunacy") != std::string::npos) {
        return "Lunacy Audio";
    }

    if (nameLower.find("bassroom") != std::string::npos ||
        nameLower.find("mixroom") != std::string::npos ||
        nameLower.find("mastering the mix") != std::string::npos) {
        return "Mastering The Mix";
    }

    if (nameLower.find("screamer") != std::string::npos ||
        nameLower.find("ablaze") != std::string::npos) {
        return "Ablaze Audio";
    }

    if (nameLower.find("dark cabin") != std::string::npos) {
        return "Dark Cabin Studios";
    }

    if (nameLower.find("toneforge") != std::string::npos ||
        nameLower.find("jst ") != std::string::npos) {
        return "Joey Sturgis Tones";
    }

    // 2. Parent directory inspection (filtering out system folders - fast path without loading DLL)
    static const std::unordered_set<std::string> s_systemDirs = {
        "vst3", "clap", "contents", "x86_64-win", "common files", "vstplugins",
        "program files", "program files (x86)", "steinberg", "plugins", "vst",
        "audio", "windows", "users", "documents", "appdata", "roaming", "local"
    };

    auto parent = path.parent_path();
    while (!parent.empty() && parent.has_filename()) {
        std::string pName = parent.filename().string();
        std::string pNameLower = pName;
        std::transform(pNameLower.begin(), pNameLower.end(), pNameLower.begin(), [](unsigned char c) { return std::tolower(c); });

        if (s_systemDirs.find(pNameLower) == s_systemDirs.end() &&
            pNameLower.find(".vst3") == std::string::npos &&
            pNameLower.find(".clap") == std::string::npos) {
            return pName;
        }
        parent = parent.parent_path();
    }

    // 3. Query VST3 factory metadata only if parent folder was generic (wrapped in SEH/VEH guard)
    std::filesystem::path dllPath = path;
    if (std::filesystem::is_directory(path)) {
        auto candidate = path / "Contents" / "x86_64-win" / (path.stem().string() + ".vst3");
        if (std::filesystem::exists(candidate)) dllPath = candidate;
    }

#if defined(_WIN32)
    HMODULE hLib = nullptr;
    const std::wstring wDllPath = dllPath.wstring();
    safeCallPlugin([&]() {
        hLib = LoadLibraryExW(wDllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hLib) {
            hLib = LoadLibraryW(wDllPath.c_str());
        }
    });
    if (hLib) {
        auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));
        std::string fVendor;
        bool hadFactory = false;
        if (getFactory) {
            char vendorBuf[128] = {0};
            safeCallPlugin([&]() {
                if (auto* factory = getFactory()) {
                    hadFactory = true;
                    Steinberg::PFactoryInfo fInfo{};
                    if (factory->getFactoryInfo(&fInfo) == Steinberg::kResultOk && fInfo.vendor[0] != '\0') {
                        std::strncpy(vendorBuf, fInfo.vendor, sizeof(vendorBuf) - 1);
                    }
                    factory->release();
                }
            });
            if (vendorBuf[0] != '\0') {
                fVendor = vendorBuf;
            }
        }
        safeCallPlugin([&]() { FreeLibrary(hLib); });
        if (!fVendor.empty()) return fVendor;
        if (hadFactory) return "Other";
    }
#else
    (void)dllPath;
#endif

    return "Other";
}

std::string detectPluginCategory(const std::string& name, const std::string& vendor) {
    std::string lower = name + " " + vendor;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Check metering/utility first so "MetricAB" does not match "cab" under Amp Emulation
    if (lower.find("metricab") != std::string::npos || lower.find("meter") != std::string::npos ||
        lower.find("scope") != std::string::npos || lower.find("util") != std::string::npos) {
        return "Utility/Metering";
    }

    if (lower.find("tune") != std::string::npos || lower.find("tuner") != std::string::npos ||
        lower.find("pitch") != std::string::npos || lower.find("scale") != std::string::npos ||
        lower.find("autotune") != std::string::npos || lower.find("vocod") != std::string::npos ||
        lower.find("harmon") != std::string::npos) {
        return "Pitch/Tuning";
    }
    if (lower.find("drive") != std::string::npos || lower.find("overdrive") != std::string::npos ||
        lower.find("dist") != std::string::npos || lower.find("fuzz") != std::string::npos ||
        lower.find("screamer") != std::string::npos || lower.find("boost") != std::string::npos ||
        lower.find("muff") != std::string::npos || lower.find("rat") != std::string::npos ||
        lower.find("klon") != std::string::npos || lower.find("od") != std::string::npos) {
        return "Distortion";
    }
    if (lower.find("amp") != std::string::npos || lower.find("archetype") != std::string::npos ||
        lower.find("neural dsp") != std::string::npos || lower.find("head") != std::string::npos ||
        lower.find("cab") != std::string::npos || lower.find("marshall") != std::string::npos ||
        lower.find("fender") != std::string::npos || lower.find("mesa") != std::string::npos ||
        lower.find("recto") != std::string::npos || lower.find("soldano") != std::string::npos ||
        lower.find("lead") != std::string::npos || lower.find("crunch") != std::string::npos ||
        lower.find("ampeg") != std::string::npos || lower.find("svt") != std::string::npos ||
        lower.find("ir") != std::string::npos) {
        return "Amp Emulation";
    }
    if (lower.find("comp") != std::string::npos || lower.find("limit") != std::string::npos ||
        lower.find("gate") != std::string::npos || lower.find("level") != std::string::npos ||
        lower.find("dynamic") != std::string::npos || lower.find("1176") != std::string::npos ||
        lower.find("la2a") != std::string::npos || lower.find("opticom") != std::string::npos) {
        return "Dynamics";
    }
    if (lower.find("eq") != std::string::npos || lower.find("filter") != std::string::npos ||
        lower.find("curve") != std::string::npos || lower.find("graphic") != std::string::npos ||
        lower.find("param") != std::string::npos) {
        return "Equalizer";
    }
    if (lower.find("delay") != std::string::npos || lower.find("echo") != std::string::npos ||
        lower.find("tape") != std::string::npos || lower.find("space") != std::string::npos ||
        lower.find("repeat") != std::string::npos || lower.find("bbd") != std::string::npos) {
        return "Delay/Echo";
    }
    if (lower.find("verb") != std::string::npos || lower.find("reverb") != std::string::npos ||
        lower.find("hall") != std::string::npos || lower.find("room") != std::string::npos ||
        lower.find("plate") != std::string::npos || lower.find("spring") != std::string::npos ||
        lower.find("shimmer") != std::string::npos) {
        return "Reverb";
    }
    if (lower.find("chorus") != std::string::npos || lower.find("flang") != std::string::npos ||
        lower.find("phas") != std::string::npos || lower.find("trem") != std::string::npos ||
        lower.find("vibe") != std::string::npos || lower.find("rotary") != std::string::npos ||
        lower.find("mod") != std::string::npos) {
        return "Modulation";
    }
    return "Audio FX";
}

void PluginScanner::scanAll() {
    if (m_isScanning.load()) return;

    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }

    m_isScanning = true;
    m_shouldStop = false;

    m_scanThread = std::thread([this]() {
        runScan();
    });
}

void PluginScanner::scanSync() {
    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
    m_isScanning = true;
    m_shouldStop = false;
    runScan();
}

void PluginScanner::runScan() {
    std::vector<PluginDescriptor> found;

    // Register built-in effects
    found.push_back(PluginDescriptor{
        .name = "Praccy Drive",
        .path = "builtin://drive",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Distortion",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });
    found.push_back(PluginDescriptor{
        .name = "Praccy Amp Sim",
        .path = "builtin://amp",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Amp Emulation",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });
    found.push_back(PluginDescriptor{
        .name = "Praccy Stereo Delay",
        .path = "builtin://delay",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Delay/Echo",
        .bundleFileName = "",
        .classGuid = "",
        .clapId = ""
    });

    std::vector<std::string> pathsCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pathsCopy = m_searchPaths;
    }

    for (const auto& dirStr : pathsCopy) {
        if (m_shouldStop.load()) break;
        std::error_code ec;
        std::filesystem::path p(dirStr);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            scanDirectory(p, found);
        }
    }

    if (!m_shouldStop.load()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& newPlug : found) {
                auto existingIt = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& old) {
                    return old.name == newPlug.name && old.path == newPlug.path;
                });
                if (existingIt != m_plugins.end()) {
                    if (newPlug.classGuid.empty() && !existingIt->classGuid.empty()) {
                        newPlug.classGuid = existingIt->classGuid;
                    }
                    if (newPlug.clapId.empty() && !existingIt->clapId.empty()) {
                        newPlug.clapId = existingIt->clapId;
                    }
                }
            }
            if (!m_plugins.empty()) {
                { if (m_retiredPluginSnapshots.size() >= 2) m_retiredPluginSnapshots.erase(m_retiredPluginSnapshots.begin()); m_retiredPluginSnapshots.push_back(std::move(m_plugins)); }
            }
            m_plugins = std::move(found);
        }
        saveToAppConfig();
    }

    m_isScanning = false;
}

void PluginScanner::scanDirectory(const std::filesystem::path& dirPath, std::vector<PluginDescriptor>& outPlugins) {
    std::error_code ec;
    try {
        for (auto iter = std::filesystem::recursive_directory_iterator(dirPath, std::filesystem::directory_options::skip_permission_denied, ec);
             iter != std::filesystem::recursive_directory_iterator(); ++iter) {
            if (m_shouldStop.load() || ec) break;

            const auto& path = iter->path();
            std::string ext = path.extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (ext == ".clap") {
                const std::string stemName = path.stem().string();
                struct ScannedClapDesc {
                    std::string id;
                    std::string name;
                    std::string vendor;
                };
                std::vector<ScannedClapDesc> foundDescs;
                bool inspected = false;

#if defined(_WIN32)
                try {
                    if (HMODULE hLib = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                        const auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(
                            reinterpret_cast<void*>(GetProcAddress(hLib, "clap_entry")));
                        const std::string pathStr = path.string();
                        bool initSuccess = false;
                        DWORD exCode = 0;
                        const bool ok = safeCallPlugin([&]() {
                            if (entry && entry->init && entry->init(pathStr.c_str())) {
                                initSuccess = true;
                                if (entry->get_factory) {
                                    const auto* fac = static_cast<const clap_plugin_factory_t*>(
                                        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
                                    if (fac && fac->get_plugin_count && fac->get_plugin_descriptor) {
                                        const uint32_t count = fac->get_plugin_count(fac);
                                        for (uint32_t cIdx = 0; cIdx < count; ++cIdx) {
                                            if (const auto* cDesc = fac->get_plugin_descriptor(fac, cIdx)) {
                                                ScannedClapDesc cd;
                                                if (cDesc->id) cd.id = cDesc->id;
                                                if (cDesc->name) cd.name = cDesc->name;
                                                if (cDesc->vendor && cDesc->vendor[0] != '\0') cd.vendor = cDesc->vendor;
                                                foundDescs.push_back(std::move(cd));
                                            }
                                        }
                                    }
                                }
                                if (entry->deinit) entry->deinit();
                            }
                        }, &exCode);
                        FreeLibrary(hLib);
                        inspected = ok && initSuccess;
                    }
                } catch (...) {
                    inspected = false;
                }
#endif

                if (inspected && !foundDescs.empty()) {
                    for (const auto& fd : foundDescs) {
                        const std::string name = fd.name.empty() ? stemName : fd.name;
                        auto itExists = std::find_if(outPlugins.begin(), outPlugins.end(), [&](const PluginDescriptor& d) {
                            return d.name == name && d.type == PluginType::CLAP;
                        });
                        if (itExists == outPlugins.end()) {
                            PluginDescriptor desc;
                            desc.name = name;
                            desc.path = path.string();
                            desc.bundleFileName = path.filename().string();
                            desc.type = PluginType::CLAP;
                            desc.clapId = fd.id;
                            desc.vendor = !fd.vendor.empty() ? fd.vendor : detectVendor(path, name);
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                auto catIt = m_categoryOverrides.find(name);
                                desc.category = (catIt != m_categoryOverrides.end()) ? catIt->second : detectPluginCategory(name, desc.vendor);
                            }
                            outPlugins.push_back(std::move(desc));
                        }
                    }
                } else {
                    auto itExists = std::find_if(outPlugins.begin(), outPlugins.end(), [&](const PluginDescriptor& d) {
                        return d.name == stemName && d.type == PluginType::CLAP;
                    });
                    if (itExists == outPlugins.end()) {
                        PluginDescriptor desc;
                        desc.name = stemName;
                        desc.path = path.string();
                        desc.bundleFileName = path.filename().string();
                        desc.type = PluginType::CLAP;
                        desc.vendor = detectVendor(path, stemName);
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            auto catIt = m_categoryOverrides.find(stemName);
                            desc.category = (catIt != m_categoryOverrides.end()) ? catIt->second : detectPluginCategory(stemName, desc.vendor);
                        }
                        outPlugins.push_back(std::move(desc));
                    }
                }
            } else if (ext == ".vst3") {
                const std::string name = path.stem().string();
                auto itExists = std::find_if(outPlugins.begin(), outPlugins.end(), [&](const PluginDescriptor& d) {
                    return d.name == name && d.type == PluginType::VST3;
                });
                if (itExists == outPlugins.end()) {
                    PluginDescriptor desc;
                    desc.name = name;
                    desc.path = path.string();
                    desc.bundleFileName = path.filename().string();
                    desc.type = PluginType::VST3;
                    std::string factoryVendor;

                    // Extract canonical 32-char VST3 Class GUID for Tier-1 PluginResolver (guarded by SEH/VEH)
#if defined(_WIN32)
                    try {
                        std::filesystem::path dllPath = path;
                        if (std::filesystem::is_directory(path)) {
                            auto candidate = path / "Contents" / "x86_64-win" / (path.stem().string() + ".vst3");
                            if (std::filesystem::exists(candidate)) dllPath = candidate;
                        }
                        if (HMODULE hLib = LoadLibraryExW(dllPath.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                            auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));
                            if (getFactory) {
                                char hex[33] = {0};
                                char vendorBuf[128] = {0};
                                bool hadFactory = false;
                                DWORD exCode = 0;
                                const bool ok = safeCallPlugin([&]() {
                                    if (auto* factory = getFactory()) {
                                        hadFactory = true;
                                        Steinberg::PFactoryInfo fInfo{};
                                        if (factory->getFactoryInfo(&fInfo) == Steinberg::kResultOk && fInfo.vendor[0] != '\0') {
                                            std::strncpy(vendorBuf, fInfo.vendor, sizeof(vendorBuf) - 1);
                                        }
                                        const int32_t nClasses = factory->countClasses();
                                        Steinberg::PClassInfo ci{};
                                        for (int32_t cIdx = 0; cIdx < nClasses; ++cIdx) {
                                            if (factory->getClassInfo(cIdx, &ci) == Steinberg::kResultOk &&
                                                std::strcmp(ci.category, "Audio Module Class") == 0) {
                                                for (int b = 0; b < 16; ++b) {
                                                    std::snprintf(hex + b * 2, 3, "%02X", static_cast<unsigned char>(ci.cid[b]));
                                                }
                                                break;
                                            }
                                        }
                                        factory->release();
                                    }
                                }, &exCode);
                                if (ok && hadFactory) {
                                    if (hex[0] != '\0') desc.classGuid = hex;
                                    if (vendorBuf[0] != '\0') factoryVendor = vendorBuf;
                                } else {
                                    std::cerr << "[PluginScanner] GetPluginFactory failed or returned nullptr for: " << dllPath.string() << "\n";
                                }
                            } else {
                                std::cerr << "[PluginScanner] GetPluginFactory not found in: " << dllPath.string() << "\n";
                            }
                            FreeLibrary(hLib);
                        } else {
                            std::cerr << "[PluginScanner] Failed to load VST3 library: " << dllPath.string() << " (error: " << GetLastError() << ")\n";
                        }
                    } catch (const std::exception& ex) {
                        std::cerr << "[PluginScanner] Exception inspecting VST3 " << path.string() << ": " << ex.what() << "\n";
                    } catch (...) {
                        std::cerr << "[PluginScanner] Unknown exception inspecting VST3 " << path.string() << "\n";
                    }
#endif

                    desc.vendor = !factoryVendor.empty() ? factoryVendor : detectVendor(path, name);
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto catIt = m_categoryOverrides.find(name);
                        desc.category = (catIt != m_categoryOverrides.end()) ? catIt->second : detectPluginCategory(name, desc.vendor);
                    }
                    outPlugins.push_back(std::move(desc));
                }
                if (iter->is_directory()) {
                    iter.disable_recursion_pending();
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "[PluginScanner] Error iterating directory " << dirPath.string() << ": " << ex.what() << "\n";
    } catch (...) {
        std::cerr << "[PluginScanner] Unknown error iterating directory " << dirPath.string() << "\n";
    }
}

bool PluginScanner::isFavorite(const std::string& nameOrPath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& fav : m_favorites) {
        if (matchesFavoriteString(fav, nameOrPath)) {
            return true;
        }
    }
    return false;
}

void PluginScanner::toggleFavorite(const std::string& nameOrPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_favoritesInitialized = true;
    auto it = std::find_if(m_favorites.begin(), m_favorites.end(), [&](const std::string& f) {
        return matchesFavoriteString(f, nameOrPath);
    });
    if (it != m_favorites.end()) {
        m_favorites.erase(it);
    } else {
        m_favorites.push_back(nameOrPath);
    }
    state::AppConfig cfg;
    cfg.load(m_configPath);
    cfg.favoritePlugins = m_favorites;
    cfg.save(m_configPath);
}

size_t PluginScanner::numFavorites() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_favorites.size();
}

std::vector<std::string> PluginScanner::favorites() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_favorites;
}

void PluginScanner::setFavorites(const std::vector<std::string>& favs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_favorites = favs;
    m_favoritesInitialized = true;
}

void PluginScanner::setCustomCategory(const std::string& pluginName, const std::string& category) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_categoryOverrides[pluginName] = category;
    for (auto& p : m_plugins) {
        if (p.name == pluginName) {
            p.category = category;
        }
    }
    state::AppConfig cfg;
    cfg.load(m_configPath);
    cfg.customPluginCategories = m_categoryOverrides;
    for (auto& sp : cfg.scannedPlugins) {
        if (sp.name == pluginName) {
            sp.category = category;
        }
    }
    cfg.save(m_configPath);
}

void PluginScanner::resetCustomCategory(const std::string& pluginName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_categoryOverrides.erase(pluginName);
    for (auto& p : m_plugins) {
        if (p.name == pluginName) {
            p.category = detectPluginCategory(p.name, p.vendor);
        }
    }
    state::AppConfig cfg;
    cfg.load(m_configPath);
    cfg.customPluginCategories = m_categoryOverrides;
    for (auto& sp : cfg.scannedPlugins) {
        if (sp.name == pluginName) {
            sp.category = detectPluginCategory(sp.name, sp.vendor);
        }
    }
    cfg.save(m_configPath);
}

std::string PluginScanner::getPluginCategory(const std::string& pluginName, const std::string& vendor) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_categoryOverrides.find(pluginName);
    if (it != m_categoryOverrides.end()) {
        return it->second;
    }
    for (const auto& p : m_plugins) {
        if (p.name == pluginName && !p.category.empty() && p.category != "VST3" && p.category != "CLAP" && p.category != "DSP") {
            return p.category;
        }
    }
    return detectPluginCategory(pluginName, vendor);
}

std::vector<std::string> PluginScanner::getDevelopers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::set<std::string> devSet;
    for (const auto& p : m_plugins) {
        if (!p.vendor.empty()) {
            devSet.insert(p.vendor);
        }
    }
    return std::vector<std::string>(devSet.begin(), devSet.end());
}

std::vector<PluginDescriptor> PluginScanner::getFilteredPlugins(
    const std::string& searchQuery,
    const std::string& developerFilter,
    const std::string& formatFilter,
    PluginSortMode sortMode,
    bool favoritesOnly
) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PluginDescriptor> result;

    std::string qLower = searchQuery;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto& p : m_plugins) {
        // Favorites filter
        if (favoritesOnly) {
            bool isFav = false;
            for (const auto& fav : m_favorites) {
                if (matchesFavoriteString(fav, p.name) || matchesFavoriteString(fav, p.path)) {
                    isFav = true;
                    break;
                }
            }
            if (!isFav) {
                continue;
            }
        }

        // Format filter
        if (!formatFilter.empty() && formatFilter != "All") {
            if (p.typeString() != formatFilter) continue;
        }

        // Developer filter
        if (!developerFilter.empty() && developerFilter != "All") {
            if (p.vendor != developerFilter) continue;
        }

        // Search query filter
        if (!qLower.empty()) {
            std::string nameLower = p.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) { return std::tolower(c); });
            std::string vendorLower = p.vendor;
            std::transform(vendorLower.begin(), vendorLower.end(), vendorLower.begin(), [](unsigned char c) { return std::tolower(c); });

            if (nameLower.find(qLower) == std::string::npos && vendorLower.find(qLower) == std::string::npos) {
                continue;
            }
        }

        result.push_back(p);
    }

    // Sort
    switch (sortMode) {
        case PluginSortMode::NameAscending:
            std::sort(result.begin(), result.end(), [](const PluginDescriptor& a, const PluginDescriptor& b) {
                return a.name < b.name;
            });
            break;
        case PluginSortMode::NameDescending:
            std::sort(result.begin(), result.end(), [](const PluginDescriptor& a, const PluginDescriptor& b) {
                return a.name > b.name;
            });
            break;
        case PluginSortMode::VendorAscending:
            std::sort(result.begin(), result.end(), [](const PluginDescriptor& a, const PluginDescriptor& b) {
                if (a.vendor == b.vendor) return a.name < b.name;
                return a.vendor < b.vendor;
            });
            break;
        case PluginSortMode::FormatAscending:
            std::sort(result.begin(), result.end(), [](const PluginDescriptor& a, const PluginDescriptor& b) {
                if (a.typeString() == b.typeString()) return a.name < b.name;
                return a.typeString() < b.typeString();
            });
            break;
    }

    return result;
}

const PluginDescriptor* PluginScanner::resolvePlugin(
    const std::string& name,
    const std::string& bundleFileName,
    const std::string& guidOrClapId,
    PluginType preferredType
) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string effectiveBundle = bundleFileName;
    if (bundleFileName == "CLAP" || bundleFileName == "clap") {
        preferredType = PluginType::CLAP;
        effectiveBundle.clear();
    } else if (bundleFileName == "VST3" || bundleFileName == "vst3") {
        preferredType = PluginType::VST3;
        effectiveBundle.clear();
    }

    // Normalize input GUID
    std::string normGuid = utils::GuidUtils::normalizeToCanonicalGuid(guidOrClapId);

    // Tier 1 (Normalized Unique ID): Canonical 32-char hex GUID comparison or CLAP plugin ID
    if (!normGuid.empty() || !guidOrClapId.empty()) {
        for (const auto& p : m_plugins) {
            if (!normGuid.empty() && !p.classGuid.empty()) {
                if (p.classGuid == normGuid) return &p;
            }
            if (!guidOrClapId.empty() && !p.clapId.empty()) {
                if (p.clapId == guidOrClapId) return &p;
            }
        }
    }

    // Tier 2 (Bundle Filename Match): Match against bundleFileName
    if (!effectiveBundle.empty()) {
        std::string bundleLower = effectiveBundle;
        std::transform(bundleLower.begin(), bundleLower.end(), bundleLower.begin(), [](unsigned char c){ return std::tolower(c); });
        std::string bundleStem = std::filesystem::path(effectiveBundle).stem().string();
        std::transform(bundleStem.begin(), bundleStem.end(), bundleStem.begin(), [](unsigned char c){ return std::tolower(c); });

        for (const auto& p : m_plugins) {
            std::string pBundleLower = p.bundleFileName;
            std::transform(pBundleLower.begin(), pBundleLower.end(), pBundleLower.begin(), [](unsigned char c){ return std::tolower(c); });
            if (!pBundleLower.empty() && pBundleLower == bundleLower) {
                return &p;
            }
            std::string pStem = std::filesystem::path(p.path).stem().string();
            std::transform(pStem.begin(), pStem.end(), pStem.begin(), [](unsigned char c){ return std::tolower(c); });
            if (!pStem.empty() && pStem == bundleStem) {
                return &p;
            }
        }
    }

    // Tier 3 (Normalized Name Match): Strip common prefixes/suffixes and match against scanned library
    if (!name.empty()) {
        auto normalizeName = [](std::string s) -> std::string {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
            for (const char* pre : {"vst3: ", "vst: ", "clap: ", "builtin: "}) {
                if (s.starts_with(pre)) s = s.substr(std::strlen(pre));
            }
            for (const char* suf : {".vst3", ".clap", " (vst3)", " (clap)", " (x64)", "_x64", " 64bit"}) {
                if (s.ends_with(suf)) s = s.substr(0, s.size() - std::strlen(suf));
            }
            std::string clean;
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c))) clean.push_back(c);
            }
            return clean;
        };

        std::string cleanInput = normalizeName(name);
        if (!cleanInput.empty()) {
            const PluginDescriptor* fallbackMatch = nullptr;
            for (const auto& p : m_plugins) {
                if (normalizeName(p.name) == cleanInput) {
                    if (p.type == preferredType) {
                        return &p;
                    }
                    if (!fallbackMatch) {
                        fallbackMatch = &p;
                    }
                }
            }
            if (fallbackMatch) {
                return fallbackMatch;
            }
        }
    }

    // Tier 4 (Fallback): Return nullptr
    return nullptr;
}

std::optional<PluginDescriptor> PluginScanner::resolvePluginCopy(
    const std::string& name,
    const std::string& bundleFileName,
    const std::string& guidOrClapId,
    PluginType preferredType
) const {
    const PluginDescriptor* ptr = resolvePlugin(name, bundleFileName, guidOrClapId, preferredType);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ptr) return std::nullopt;
    return *ptr;
}

} // namespace praccy::plugins
