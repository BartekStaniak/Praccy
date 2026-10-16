#include "plugin_browser_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include "../../state/app_config.h"
#include "../../plugins/builtin_dsp.h"
#include "../../plugins/vst3_host.h"
#include "../../plugins/clap_host.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <memory>

namespace praccy::ui {

namespace {

std::string toLower(std::string_view str) {
    std::string out(str);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string cleanPluginName(std::string_view rawName) {
    std::string name(rawName);
    if (name.size() >= 5) {
        std::string ext = name.substr(name.size() - 5);
        std::string extLower = toLower(ext);
        if (extLower == ".vst3" || extLower == ".clap") {
            name.resize(name.size() - 5);
        }
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

int calculateFuzzyScore(std::string_view query, const plugins::PluginDescriptor& desc) {
    if (query.empty()) return 0;

    std::string q = toLower(query);
    std::string cleanName = toLower(cleanPluginName(desc.name));
    std::string rawName = toLower(desc.name);
    std::string vendor = toLower(desc.vendor);
    std::string cat = toLower(desc.category);
    std::string format = toLower(desc.typeString());

    int score = 0;

    // Exact name match
    if (cleanName == q || rawName == q) {
        return 2000;
    }

    // Prefix name match
    if (cleanName.rfind(q, 0) == 0 || rawName.rfind(q, 0) == 0) {
        score += 500;
    }
    // Word boundary match in name
    else if (cleanName.find(" " + q) != std::string::npos || cleanName.find("-" + q) != std::string::npos || cleanName.find("_" + q) != std::string::npos) {
        score += 300;
    }
    // Substring in name
    else if (cleanName.find(q) != std::string::npos || rawName.find(q) != std::string::npos) {
        score += 200;
    }

    // Substring in vendor
    if (vendor.find(q) != std::string::npos) {
        score += 120;
    }

    // Substring in category
    if (cat.find(q) != std::string::npos) {
        score += 80;
    }

    // Substring in format type
    if (format.find(q) != std::string::npos) {
        score += 60;
    }

    // Subsequence fuzzy search in cleanName
    size_t qIdx = 0;
    int consecutive = 0;
    int subseqScore = 0;
    for (size_t i = 0; i < cleanName.length() && qIdx < q.length(); ++i) {
        if (cleanName[i] == q[qIdx]) {
            qIdx++;
            consecutive++;
            subseqScore += 15 + (consecutive * 5);
        } else {
            consecutive = 0;
        }
    }

    if (qIdx == q.length()) {
        score = std::max(score, subseqScore);
    }

    if (cleanName != q && score >= 2000) {
        score = 1999;
    }

    return score;
}

} // namespace

PluginBrowserModal::PluginBrowserModal(plugins::PluginScanner& scanner, audio::GraphEngine& graph)
    : m_scanner(scanner)
    , m_graph(graph)
{
    state::AppConfig cfg;
    if (cfg.load()) {
        m_recentPlugins = cfg.recentPlugins;
        m_favoritePlugins = cfg.favoritePlugins;
    }
}

void PluginBrowserModal::open(int targetBlockIndex, int targetBranchIndex) {
    m_targetBlockIndex = targetBlockIndex;
    m_targetBranchIndex = targetBranchIndex;
    m_isOpen = true;
    m_justOpened = true;
    m_needsFocus = true;
    m_scrollSelectionIntoView = true;
    m_selectedIndex = 0;
    m_searchQuery[0] = '\0';
    m_lastSearchQuery[0] = '\0';
    m_currentScope = PaletteScope::All;

    state::AppConfig cfg;
    if (cfg.load()) {
        m_recentPlugins = cfg.recentPlugins;
        m_favoritePlugins = cfg.favoritePlugins;
    }

    m_allPlugins = m_scanner.scannedPlugins();
    updateFilteredList();
}

void PluginBrowserModal::close() {
    m_isOpen = false;
    m_searchQuery[0] = '\0';
    m_lastSearchQuery[0] = '\0';
}

void PluginBrowserModal::navigateSelection(int delta) {
    if (m_filteredItems.empty()) return;
    int count = static_cast<int>(m_filteredItems.size());
    m_selectedIndex = (m_selectedIndex + delta + count) % count;
    m_scrollSelectionIntoView = true;
}

void PluginBrowserModal::setScope(PaletteScope scope) {
    if (m_currentScope != scope) {
        m_currentScope = scope;
        m_selectedIndex = 0;
        m_scrollSelectionIntoView = true;
        updateFilteredList();
    }
}

void PluginBrowserModal::cycleScope() {
    uint8_t next = (static_cast<uint8_t>(m_currentScope) + 1) % 3;
    setScope(static_cast<PaletteScope>(next));
}

bool PluginBrowserModal::isFavorite(const std::string& pluginName) const {
    std::string clean = cleanPluginName(pluginName);
    for (const auto& fav : m_favoritePlugins) {
        if (fav == pluginName || cleanPluginName(fav) == clean) {
            return true;
        }
    }
    return false;
}

void PluginBrowserModal::toggleFavorite(const std::string& pluginName) {
    std::string targetClean = cleanPluginName(pluginName);
    auto it = std::find_if(m_favoritePlugins.begin(), m_favoritePlugins.end(), [&](const std::string& f) {
        return f == pluginName || cleanPluginName(f) == targetClean;
    });

    if (it != m_favoritePlugins.end()) {
        m_favoritePlugins.erase(it);
    } else {
        m_favoritePlugins.push_back(pluginName);
    }

    state::AppConfig cfg;
    cfg.load();
    cfg.favoritePlugins = m_favoritePlugins;
    cfg.save();

    updateFilteredList();
}

void PluginBrowserModal::toggleFavoriteForSelected() {
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredItems.size())) {
        const auto* desc = m_filteredItems[m_selectedIndex].descriptor;
        if (desc) {
            toggleFavorite(desc->name);
        }
    }
}

int PluginBrowserModal::SearchInputCallback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<PluginBrowserModal*>(data->UserData);
    if (!self) return 0;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (data->EventKey == ImGuiKey_UpArrow) {
            self->navigateSelection(-1);
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            self->navigateSelection(1);
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        self->cycleScope();
        self->m_tabHandledThisFrame = true;
    }
    return 0;
}

void PluginBrowserModal::updateFilteredList() {
    m_filteredItems.clear();
    m_allPlugins = m_scanner.scannedPlugins();
    const auto& allPlugins = m_allPlugins;

    auto matchesScope = [this](const plugins::PluginDescriptor& p) -> bool {
        if (m_currentScope == PaletteScope::Favorites) {
            return isFavorite(p.name);
        } else if (m_currentScope == PaletteScope::Recents) {
            return std::find(m_recentPlugins.begin(), m_recentPlugins.end(), p.name) != m_recentPlugins.end();
        }
        return true;
    };

    if (m_searchQuery[0] == '\0') {
        // Zero-query pinned sorting
        if (m_currentScope == PaletteScope::All) {
            // 1. Favorites pinned at top
            std::vector<const plugins::PluginDescriptor*> favorites;
            for (const auto& favName : m_favoritePlugins) {
                std::string cleanFav = cleanPluginName(favName);
                for (const auto& p : allPlugins) {
                    if (p.name == favName || cleanPluginName(p.name) == cleanFav) {
                        if (std::find(favorites.begin(), favorites.end(), &p) == favorites.end()) {
                            favorites.push_back(&p);
                        }
                        break;
                    }
                }
            }
            std::sort(favorites.begin(), favorites.end(), [](const auto* a, const auto* b) {
                return a->name < b->name;
            });
            for (const auto* p : favorites) {
                PaletteItem item;
                item.descriptor = p;
                item.score = 2000;
                item.isFavorite = true;
                item.isRecent = (std::find(m_recentPlugins.begin(), m_recentPlugins.end(), p->name) != m_recentPlugins.end());
                m_filteredItems.push_back(item);
            }

            // 2. Recents that are not already in Favorites
            std::vector<const plugins::PluginDescriptor*> recents;
            for (const auto& recentName : m_recentPlugins) {
                for (const auto& p : allPlugins) {
                    if (p.name == recentName) {
                        bool alreadyAdded = std::any_of(m_filteredItems.begin(), m_filteredItems.end(), [&](const PaletteItem& it) {
                            return it.descriptor == &p;
                        });
                        if (!alreadyAdded) {
                            recents.push_back(&p);
                        }
                        break;
                    }
                }
            }
            for (const auto* p : recents) {
                PaletteItem item;
                item.descriptor = p;
                item.score = 1000;
                item.isFavorite = isFavorite(p->name);
                item.isRecent = true;
                m_filteredItems.push_back(item);
            }

            // 3. All remaining plugins sorted alphabetically
            std::vector<const plugins::PluginDescriptor*> remaining;
            for (const auto& p : allPlugins) {
                bool alreadyAdded = std::any_of(m_filteredItems.begin(), m_filteredItems.end(), [&](const PaletteItem& it) {
                    return it.descriptor == &p;
                });
                if (!alreadyAdded) {
                    remaining.push_back(&p);
                }
            }
            std::sort(remaining.begin(), remaining.end(), [](const auto* a, const auto* b) {
                return a->name < b->name;
            });
            for (const auto* p : remaining) {
                PaletteItem item;
                item.descriptor = p;
                item.score = 0;
                item.isFavorite = isFavorite(p->name);
                item.isRecent = false;
                m_filteredItems.push_back(item);
            }
        } else if (m_currentScope == PaletteScope::Favorites) {
            std::vector<const plugins::PluginDescriptor*> favList;
            for (const auto& p : allPlugins) {
                if (isFavorite(p.name)) {
                    favList.push_back(&p);
                }
            }
            std::sort(favList.begin(), favList.end(), [](const auto* a, const auto* b) {
                return a->name < b->name;
            });
            for (const auto* p : favList) {
                PaletteItem item;
                item.descriptor = p;
                item.score = 0;
                item.isFavorite = true;
                item.isRecent = (std::find(m_recentPlugins.begin(), m_recentPlugins.end(), p->name) != m_recentPlugins.end());
                m_filteredItems.push_back(item);
            }
        } else if (m_currentScope == PaletteScope::Recents) {
            for (const auto& recentName : m_recentPlugins) {
                for (const auto& p : allPlugins) {
                    if (p.name == recentName) {
                        PaletteItem item;
                        item.descriptor = &p;
                        item.score = 0;
                        item.isFavorite = isFavorite(p.name);
                        item.isRecent = true;
                        m_filteredItems.push_back(item);
                        break;
                    }
                }
            }
        }
    } else {
        // Query active: fuzzy filter across name, vendor, category, format
        for (const auto& p : allPlugins) {
            if (!matchesScope(p)) continue;

            int score = calculateFuzzyScore(m_searchQuery, p);
            if (score > 0) {
                bool fav = isFavorite(p.name);
                if (fav) {
                    score += 250; // Search score boost for favorites
                }
                PaletteItem item;
                item.descriptor = &p;
                item.score = score;
                item.isFavorite = fav;
                item.isRecent = (std::find(m_recentPlugins.begin(), m_recentPlugins.end(), p.name) != m_recentPlugins.end());
                m_filteredItems.push_back(item);
            }
        }

        std::sort(m_filteredItems.begin(), m_filteredItems.end(), [](const PaletteItem& a, const PaletteItem& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.descriptor->name < b.descriptor->name;
        });
    }

    if (m_selectedIndex >= static_cast<int>(m_filteredItems.size())) {
        m_selectedIndex = std::max(0, static_cast<int>(m_filteredItems.size()) - 1);
    }
}

void PluginBrowserModal::recordPluginUsage(const plugins::PluginDescriptor& desc) {
    auto it = std::find(m_recentPlugins.begin(), m_recentPlugins.end(), desc.name);
    if (it != m_recentPlugins.end()) {
        m_recentPlugins.erase(it);
    }
    m_recentPlugins.insert(m_recentPlugins.begin(), desc.name);
    if (m_recentPlugins.size() > 8) {
        m_recentPlugins.resize(8);
    }

    state::AppConfig cfg;
    cfg.load();
    cfg.recentPlugins = m_recentPlugins;
    cfg.save();
}

void PluginBrowserModal::instantiateSelectedPlugin() {
    if (m_filteredItems.empty() || m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_filteredItems.size())) {
        return;
    }

    const auto* desc = m_filteredItems[m_selectedIndex].descriptor;
    if (!desc) return;

    std::unique_ptr<audio::AudioNode> innerNode;
    if (desc->type == plugins::PluginType::VST3) {
        innerNode = plugins::Vst3PluginInstance::loadFromFile(desc->path);
    } else if (desc->type == plugins::PluginType::CLAP) {
        innerNode = plugins::ClapPluginInstance::loadFromFile(desc->path);
    } else {
        if (desc->path == "builtin://drive" || desc->name.find("Drive") != std::string::npos || desc->name.find("Overdrive") != std::string::npos) {
            innerNode = std::make_unique<plugins::OverdriveEffect>();
        } else if (desc->path == "builtin://amp" || desc->name.find("Amp") != std::string::npos) {
            innerNode = std::make_unique<plugins::TubeAmpEffect>();
        } else if (desc->path == "builtin://delay" || desc->name.find("Delay") != std::string::npos) {
            innerNode = std::make_unique<plugins::StereoDelayEffect>();
        } else {
            innerNode = std::make_unique<plugins::OverdriveEffect>();
        }
    }

    if (!innerNode) {
        close();
        return;
    }

    auto slot = std::make_unique<audio::PluginSlot>(std::move(innerNode));
    std::string pluginName = desc->name;

    if (m_targetBranchIndex != -1 && m_targetBlockIndex != -1) {
        auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(m_targetBlockIndex));
        if (block) {
            auto* branch = block->getBranch(m_targetBranchIndex);
            if (branch) {
                branch->addSlot(std::move(slot));
            }
        }
    } else {
        m_graph.addSerialNode(std::move(slot));
    }

    recordPluginUsage(*desc);

    if (m_onPluginInserted) {
        m_onPluginInserted(pluginName);
    }

    close();
}

void PluginBrowserModal::renderPluginRow(int index, float rowH) {
    if (index < 0 || index >= static_cast<int>(m_filteredItems.size())) return;

    const auto& tokens = themeTokens();
    const auto& item = m_filteredItems[index];
    const auto* desc = item.descriptor;
    if (!desc) return;

    const bool isSelected = (index == m_selectedIndex);
    ImGui::PushID(index);

    ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Selectable background row
    char selId[32];
    std::snprintf(selId, sizeof(selId), "##SelRow_%d", index);
    bool clicked = ImGui::Selectable(selId, isSelected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, rowH));

    if (isSelected) {
        dl->AddRectFilled(rowPos, ImVec2(rowPos.x + rowW, rowPos.y + rowH), tokens.surfaces.cardBgSelected, 4.0f);
        // Left focus indicator bar
        dl->AddRectFilled(rowPos, ImVec2(rowPos.x + 3.0f, rowPos.y + rowH), tokens.borders.focus, 2.0f);
    }

    // Row contents with unified vertical centering
    const float starSz = 20.0f;
    const float badgeH = 18.0f;
    const float fontH = ImGui::GetFontSize();

    // 1. Star / Favorite Button (centered in rowH)
    float curX = rowPos.x + 8.0f;
    float starY = rowPos.y + (rowH - starSz) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(curX, starY));
    char starId[32];
    std::snprintf(starId, sizeof(starId), "##Star_%d", index);
    if (renderCenteredStarButton(starId, ImVec2(starSz, starSz), item.isFavorite)) {
        toggleFavorite(desc->name);
    }
    curX += starSz + 8.0f;

    // 2. Format Badge (centered in rowH)
    const char* badgeStr = "DSP";
    ImU32 badgeText = tokens.text.badgeInternal;
    if (desc->type == plugins::PluginType::VST3) {
        badgeStr = "VST3";
        badgeText = tokens.text.badgeVst3;
    } else if (desc->type == plugins::PluginType::CLAP) {
        badgeStr = "CLAP";
        badgeText = tokens.text.badgeClap;
    }
    float badgeY = rowPos.y + (rowH - badgeH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(curX, badgeY));
    renderBadgePill(badgeStr, tokens.surfaces.frameBg, tokens.borders.subtle, badgeText, badgeH);
    ImVec2 badgeSz = ImGui::CalcTextSize(badgeStr);
    curX += (badgeSz.x + 12.0f) + 10.0f;

    // 3. Clean Plugin Title (suffix clutter stripped)
    float textY = rowPos.y + (rowH - fontH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(curX, textY));
    std::string displayName = cleanPluginName(desc->name);
    ImVec4 titleCol = isSelected ? tokens.text.accent.vec4 : tokens.text.primary.vec4;
    ImGui::TextColored(titleCol, "%s", displayName.c_str());

    // 4. Vendor / Developer
    if (!desc->vendor.empty()) {
        ImGui::SameLine(0, 10.0f);
        ImGui::TextColored(tokens.text.secondary.vec4, "%s", desc->vendor.c_str());
    }

    // 5. Intelligent Type/Category Capsule on the far right
    std::string catName = m_scanner.getPluginCategory(desc->name, desc->vendor);
    bool pillHovered = false;
    if (!catName.empty()) {
        ImVec2 catSz = ImGui::CalcTextSize(catName.c_str());
        const float catPillW = catSz.x + 12.0f;
        float catX = rowPos.x + rowW - catPillW - 14.0f;
        if (catX > ImGui::GetCursorScreenPos().x + 20.0f) {
            float catY = rowPos.y + (rowH - badgeH) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(catX, catY));
            char catBtnId[64];
            std::snprintf(catBtnId, sizeof(catBtnId), "##CatPill_%d", index);
            ImGui::InvisibleButton(catBtnId, ImVec2(catPillW, badgeH));
            pillHovered = ImGui::IsItemHovered();
            if (pillHovered) {
                ImGui::SetTooltip("Category: %s\nRight-click to change category", catName.c_str());
            }

            ImGui::SetCursorScreenPos(ImVec2(catX, catY));
            renderBadgePill(catName.c_str(), tokens.surfaces.frameBg,
                            pillHovered ? tokens.borders.focus : tokens.borders.subtle,
                            pillHovered ? tokens.text.primary : tokens.text.muted, badgeH);

            if (ImGui::BeginPopupContextItem(catBtnId, ImGuiPopupFlags_MouseButtonRight)) {
                ImGui::TextDisabled("Category for %s:", desc->name.c_str());
                ImGui::Separator();
                const char* kCategories[] = {
                    "Amp Emulation",
                    "Distortion",
                    "Dynamics",
                    "Equalizer",
                    "Delay/Echo",
                    "Reverb",
                    "Modulation",
                    "Pitch/Tuning",
                    "Utility/Metering",
                    "Audio FX"
                };
                for (const char* c : kCategories) {
                    bool isCur = (catName == c);
                    if (ImGui::MenuItem(c, nullptr, isCur)) {
                        m_scanner.setCustomCategory(desc->name, c);
                        updateFilteredList();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset to Auto-detected")) {
                    m_scanner.resetCustomCategory(desc->name);
                    updateFilteredList();
                }
                ImGui::EndPopup();
            }
        }
    }

    if (clicked && !pillHovered) {
        m_selectedIndex = index;
        instantiateSelectedPlugin();
    }

    ImGui::PopID();
}

void PluginBrowserModal::render() {
    if (!m_isOpen) return;

    const auto& tokens = themeTokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const float fontSize = ImGui::GetFontSize();
    const float dpiScale = std::max(1.0f, fontSize / 13.0f);
    const float modalW = std::clamp(vp->Size.x * 0.55f, 620.0f * dpiScale, 860.0f * dpiScale);
    const float modalH = std::clamp(vp->Size.y * 0.65f, 480.0f * dpiScale, 720.0f * dpiScale);
    const ImVec2 modalSize(modalW, modalH);

    ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.focus.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * dpiScale, 14.0f * dpiScale));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("##SpotlightCommandPalette", &m_isOpen, flags)) {
        // Click outside palette closes it
        if (m_justOpened) {
            m_justOpened = false;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                close();
                ImGui::End();
                m_tabHandledThisFrame = false;
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(2);
                return;
            }
        }

        // Global shortcut Esc to close
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            return;
        }

        // Tab key scope cycling
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !m_tabHandledThisFrame) {
            cycleScope();
        }

        // F key favorite toggle
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            if (ImGui::GetIO().KeyCtrl || !ImGui::GetIO().WantTextInput) {
                toggleFavoriteForSelected();
            }
        }

        // Arrow navigation
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            navigateSelection(-1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            navigateSelection(1);
        }

        // Scope Switcher Tabs
        {
            const float tabPadX = 10.0f * dpiScale;
            const float tabPadY = 4.0f * dpiScale;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(tabPadX, tabPadY));

            auto renderScopeTab = [&](PaletteScope scope, const char* label) {
                bool isActive = (m_currentScope == scope);
                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button, tokens.surfaces.frameBgActive.vec4);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tokens.surfaces.frameBgActive.vec4);
                    ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.accent.vec4);
                    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.focus.vec4);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, tokens.surfaces.frameBg.vec4);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tokens.surfaces.frameBgHovered.vec4);
                    ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.secondary.vec4);
                    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, isActive ? 1.5f : 1.0f);

                if (ImGui::Button(label)) {
                    setScope(scope);
                }

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
            };

            renderScopeTab(PaletteScope::All, "All");
            ImGui::SameLine(0, 6.0f);

            char favLabel[48];
            std::snprintf(favLabel, sizeof(favLabel), "Favorites (%zu)", m_favoritePlugins.size());
            renderScopeTab(PaletteScope::Favorites, favLabel);
            ImGui::SameLine(0, 6.0f);

            char recLabel[48];
            std::snprintf(recLabel, sizeof(recLabel), "Recents (%zu)", m_recentPlugins.size());
            renderScopeTab(PaletteScope::Recents, recLabel);

            ImGui::PopStyleVar();
        }

        ImGui::Spacing();

        // Search Input Box
        const float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(availWidth);
        if (m_needsFocus) {
            ImGui::SetKeyboardFocusHere();
            m_needsFocus = false;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * dpiScale, 8.0f * dpiScale));
        bool enterPressed = ImGui::InputTextWithHint(
            "##PaletteSearch",
            "Search plugins by name, vendor, category, or format...",
            m_searchQuery,
            sizeof(m_searchQuery),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion,
            SearchInputCallback,
            this
        );
        ImGui::PopStyleVar();

        if (std::strcmp(m_searchQuery, m_lastSearchQuery) != 0) {
            std::snprintf(m_lastSearchQuery, sizeof(m_lastSearchQuery), "%s", m_searchQuery);
            m_selectedIndex = 0;
            updateFilteredList();
        }

        if (enterPressed || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            instantiateSelectedPlugin();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            return;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Results List Container (Virtualization via ImGuiListClipper) - dynamically scales with window height
        const float footerH = ImGui::GetTextLineHeightWithSpacing() + 14.0f * dpiScale;
        const float listHeight = std::max(140.0f * dpiScale, ImGui::GetContentRegionAvail().y - footerH);
        const float rowH = 34.0f * dpiScale;
        ImGui::BeginChild("##PaletteResultsList", ImVec2(0.0f, listHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (m_filteredItems.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored(tokens.text.muted.vec4, "No matching audio plugins found.");
        } else {
            if (m_scrollSelectionIntoView) {
                float targetScrollY = m_selectedIndex * rowH - (listHeight - rowH) * 0.5f;
                if (targetScrollY < 0.0f) targetScrollY = 0.0f;
                ImGui::SetScrollY(targetScrollY);
                m_scrollSelectionIntoView = false;
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_filteredItems.size()), rowH);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    renderPluginRow(i, rowH);
                }
            }
            clipper.End();
        }
        ImGui::EndChild();

        // High-DPI collision-free Footer Status Bar
        ImGui::Spacing();
        const char* navHint = "[UP/DOWN] Navigate    [ENTER] Insert    [F] Star    [TAB] Scope    [ESC] Dismiss";
        char countBuf[64];
        std::snprintf(countBuf, sizeof(countBuf), "%zu plugins available", m_filteredItems.size());

        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 navSize = ImGui::CalcTextSize(navHint);
        const ImVec2 countSize = ImGui::CalcTextSize(countBuf);

        ImGui::TextColored(tokens.text.muted.vec4, "%s", navHint);
        if (contentWidth >= navSize.x + countSize.x + 16.0f * dpiScale) {
            ImGui::SameLine(contentWidth - countSize.x);
            ImGui::TextColored(tokens.text.muted.vec4, "%s", countBuf);
        }
    }
    ImGui::End();

    m_tabHandledThisFrame = false;

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace praccy::ui
