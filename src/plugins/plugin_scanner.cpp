#include "plugin_scanner.h"
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <set>
#include <windows.h>
#include <pluginterfaces/base/ipluginbase.h>

namespace praccy::plugins {

using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API *)();

PluginScanner::PluginScanner() {
    addDefaultPaths();
    scanAll();
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
        nameLower.find("shadow hills") != std::string::npos) {
        return "Plugin Alliance";
    }

    if (nameLower.find("toneforge") != std::string::npos ||
        nameLower.find("jst ") != std::string::npos) {
        return "Joey Sturgis Tones";
    }

    // 2. Parent directory inspect (if inside subfolder like "Vendor/plugin.vst3")
    auto parent = path.parent_path();
    while (!parent.empty() && parent.has_filename()) {
        std::string pName = parent.filename().string();
        if (pName != "VST3" && pName != "CLAP" && pName != "Contents" && pName != "x86_64-win" &&
            pName.find(".vst3") == std::string::npos && pName.find(".clap") == std::string::npos &&
            pName != "Common Files" && pName != "VstPlugins") {
            return pName;
        }
        parent = parent.parent_path();
    }

    // 3. Fallback: query VST3 factory if possible
    std::filesystem::path dllPath = path;
    if (std::filesystem::is_directory(path)) {
        auto candidate = path / "Contents" / "x86_64-win" / (path.stem().string() + ".vst3");
        if (std::filesystem::exists(candidate)) dllPath = candidate;
    }

    HMODULE hLib = LoadLibraryExW(dllPath.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!hLib) {
        hLib = LoadLibraryW(dllPath.wstring().c_str());
    }
    if (hLib) {
        auto* getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(hLib, "GetPluginFactory"));
        if (getFactory) {
            auto* factory = getFactory();
            if (factory) {
                Steinberg::PFactoryInfo fInfo{};
                if (factory->getFactoryInfo(&fInfo) == Steinberg::kResultOk && fInfo.vendor[0] != '\0') {
                    FreeLibrary(hLib);
                    return fInfo.vendor;
                }
            }
        }
        FreeLibrary(hLib);
    }

    return "Other";
}

void PluginScanner::scanAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isScanning = true;
    m_plugins.clear();

    // Register built-in effects
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Drive",
        .path = "builtin://drive",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Distortion"
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Amp Sim",
        .path = "builtin://amp",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Amp Emulation"
    });
    m_plugins.push_back(PluginDescriptor{
        .name = "Praccy Stereo Delay",
        .path = "builtin://delay",
        .type = PluginType::BuiltIn,
        .vendor = "Praccy Audio",
        .category = "Delay/Echo"
    });

    for (const auto& dirStr : m_searchPaths) {
        std::error_code ec;
        std::filesystem::path p(dirStr);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            scanDirectory(p);
        }
    }

    m_isScanning = false;
}

void PluginScanner::scanDirectory(const std::filesystem::path& dirPath) {
    std::error_code ec;
    for (auto iter = std::filesystem::recursive_directory_iterator(dirPath, std::filesystem::directory_options::skip_permission_denied, ec);
         iter != std::filesystem::recursive_directory_iterator(); ++iter) {
        if (ec) break;

        const auto& path = iter->path();
        const std::string ext = path.extension().string();

        if (ext == ".clap") {
            const std::string name = path.stem().string();
            auto itExists = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& d) {
                return d.name == name;
            });
            if (itExists == m_plugins.end()) {
                PluginDescriptor desc;
                desc.name = name;
                desc.path = path.string();
                desc.type = PluginType::CLAP;
                desc.vendor = detectVendor(path, name);
                desc.category = "CLAP";
                m_plugins.push_back(std::move(desc));
            }
        } else if (ext == ".vst3") {
            const std::string name = path.stem().string();
            auto itExists = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginDescriptor& d) {
                return d.name == name;
            });
            if (itExists == m_plugins.end()) {
                PluginDescriptor desc;
                desc.name = name;
                desc.path = path.string();
                desc.type = PluginType::VST3;
                desc.vendor = detectVendor(path, name);
                desc.category = "VST3";
                m_plugins.push_back(std::move(desc));
            }
            if (iter->is_directory()) {
                iter.disable_recursion_pending();
            }
        }
    }
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
    PluginSortMode sortMode
) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PluginDescriptor> result;

    std::string qLower = searchQuery;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto& p : m_plugins) {
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

} // namespace praccy::plugins
