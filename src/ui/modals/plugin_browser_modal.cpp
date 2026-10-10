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

namespace praccy::ui {

namespace {

std::string toLower(std::string_view str) {
    std::string out(str);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

int calculateFuzzyScore(std::string_view query, const plugins::PluginDescriptor& desc) {
    if (query.empty()) return 0;

    std::string q = toLower(query);
    std::string name = toLower(desc.name);
    std::string vendor = toLower(desc.vendor);
    std::string cat = toLower(desc.category);
    std::string format = toLower(desc.typeString());

    int score = 0;

    // Exact name match
    if (name == q) {
        return 2000;
    }

    // Prefix name match
    if (name.rfind(q, 0) == 0) {
        score += 500;
    }
    // Word boundary match in name
    else if (name.find(" " + q) != std::string::npos || name.find("-" + q) != std::string::npos || name.find("_" + q) != std::string::npos) {
        score += 300;
    }
    // Substring in name
    else if (name.find(q) != std::string::npos) {
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

    // Subsequence fuzzy search in name (characters appear in order)
    size_t qIdx = 0;
    int consecutive = 0;
    int subseqScore = 0;
    for (size_t i = 0; i < name.length() && qIdx < q.length(); ++i) {
        if (name[i] == q[qIdx]) {
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

    if (name != q && score >= 2000) {
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
    }
}

void PluginBrowserModal::open(int targetBlockIndex, int targetBranchIndex) {
    m_targetBlockIndex = targetBlockIndex;
    m_targetBranchIndex = targetBranchIndex;
    m_isOpen = true;
    m_needsFocus = true;
    m_scrollSelectionIntoView = true;
    m_selectedIndex = 0;
    m_searchQuery[0] = '\0';
    m_lastSearchQuery[0] = '\0';
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

int PluginBrowserModal::SearchInputCallback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<PluginBrowserModal*>(data->UserData);
    if (!self) return 0;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (data->EventKey == ImGuiKey_UpArrow) {
            self->navigateSelection(-1);
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            self->navigateSelection(1);
        }
    }
    return 0;
}

void PluginBrowserModal::updateFilteredList() {
    m_filteredItems.clear();
    const auto& allPlugins = m_allPlugins;

    if (m_searchQuery[0] == '\0') {
        // Empty query: Recents first, then all alphabetically
        // 1. Recents
        for (const auto& recentName : m_recentPlugins) {
            for (const auto& p : allPlugins) {
                if (p.name == recentName) {
                    PaletteItem item;
                    item.descriptor = &p;
                    item.score = 1000;
                    item.isRecent = true;
                    m_filteredItems.push_back(item);
                    break;
                }
            }
        }

        // 2. Remaining plugins sorted alphabetically
        std::vector<const plugins::PluginDescriptor*> remaining;
        for (const auto& p : allPlugins) {
            bool inRecents = std::any_of(m_filteredItems.begin(), m_filteredItems.end(), [&](const PaletteItem& it) {
                return it.descriptor == &p;
            });
            if (!inRecents) {
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
            item.isRecent = false;
            m_filteredItems.push_back(item);
        }
    } else {
        // Query active: fuzzy filter across name, vendor, category, format
        for (const auto& p : allPlugins) {
            int score = calculateFuzzyScore(m_searchQuery, p);
            if (score > 0) {
                PaletteItem item;
                item.descriptor = &p;
                item.score = score;
                item.isRecent = false;
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

void PluginBrowserModal::render() {
    if (!m_isOpen) return;

    const auto& tokens = themeTokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const ImVec2 modalSize(560.0f, 420.0f);
    ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.focus.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("##SpotlightCommandPalette", &m_isOpen, flags)) {
        // Global shortcut Esc to close
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            return;
        }

        // Arrow navigation
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            navigateSelection(-1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            navigateSelection(1);
        }

        // Header: Search Input Box
        ImGui::SetNextItemWidth(modalSize.x - 28.0f);
        if (m_needsFocus) {
            ImGui::SetKeyboardFocusHere();
            m_needsFocus = false;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
        bool enterPressed = ImGui::InputTextWithHint(
            "##PaletteSearch",
            "Search plugins by name, vendor, category, or format...",
            m_searchQuery,
            sizeof(m_searchQuery),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
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

        // Results List Container
        const float listHeight = 300.0f;
        ImGui::BeginChild("##PaletteResultsList", ImVec2(0.0f, listHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (m_filteredItems.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored(tokens.text.muted.vec4, "No matching audio plugins found.");
        } else {
            bool renderedRecentsHeader = false;
            bool renderedAllHeader = false;
            const bool isDefaultListing = (m_searchQuery[0] == '\0');

            for (size_t i = 0; i < m_filteredItems.size(); ++i) {
                const auto& item = m_filteredItems[i];
                const auto* desc = item.descriptor;
                if (!desc) continue;

                // Section headers when search query is empty
                if (isDefaultListing) {
                    if (item.isRecent && !renderedRecentsHeader) {
                        ImGui::TextColored(tokens.text.accent.vec4, "RECENTS & FREQUENT");
                        ImGui::Separator();
                        renderedRecentsHeader = true;
                    } else if (!item.isRecent && !renderedAllHeader) {
                        if (renderedRecentsHeader) ImGui::Spacing();
                        ImGui::TextColored(tokens.text.secondary.vec4, "ALL PLUGINS (A-Z)");
                        ImGui::Separator();
                        renderedAllHeader = true;
                    }
                }

                const bool isSelected = (static_cast<int>(i) == m_selectedIndex);
                if (isSelected && m_scrollSelectionIntoView) {
                    ImGui::SetScrollHereY(0.5f);
                    m_scrollSelectionIntoView = false;
                }

                ImGui::PushID(static_cast<int>(i));
                ImVec2 rowPos = ImGui::GetCursorScreenPos();
                const float rowW = ImGui::GetContentRegionAvail().x;
                const float rowH = 34.0f;

                ImDrawList* dl = ImGui::GetWindowDrawList();

                // Selectable background
                char selId[32];
                std::snprintf(selId, sizeof(selId), "##SelRow_%zu", i);
                bool clicked = ImGui::Selectable(selId, isSelected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowH));

                if (isSelected) {
                    dl->AddRectFilled(rowPos, ImVec2(rowPos.x + rowW, rowPos.y + rowH), tokens.surfaces.cardBgSelected, 4.0f);
                    // Left focus indicator bar
                    dl->AddRectFilled(rowPos, ImVec2(rowPos.x + 3.0f, rowPos.y + rowH), tokens.borders.focus, 2.0f);
                }

                // Render row content
                ImGui::SameLine(8.0f);

                // Format badge
                const char* badgeStr = "DSP";
                ImU32 badgeText = tokens.text.badgeInternal;
                if (desc->type == plugins::PluginType::VST3) {
                    badgeStr = "VST3";
                    badgeText = tokens.text.badgeVst3;
                } else if (desc->type == plugins::PluginType::CLAP) {
                    badgeStr = "CLAP";
                    badgeText = tokens.text.badgeClap;
                }
                renderBadgePill(badgeStr, tokens.surfaces.frameBg, tokens.borders.subtle, badgeText, 18.0f);

                // Plugin Title
                ImGui::SameLine(0, 8.0f);
                ImGui::AlignTextToFramePadding();
                ImVec4 titleCol = isSelected ? tokens.text.accent.vec4 : tokens.text.primary.vec4;
                ImGui::TextColored(titleCol, "%s", desc->name.c_str());

                // Vendor / Developer
                ImGui::SameLine(0, 10.0f);
                ImGui::TextColored(tokens.text.secondary.vec4, "%s", desc->vendor.c_str());

                // Category pill on right
                if (!desc->category.empty()) {
                    ImVec2 catSz = ImGui::CalcTextSize(desc->category.c_str());
                    float catX = rowPos.x + rowW - catSz.x - 22.0f;
                    if (catX > ImGui::GetCursorScreenPos().x + 20.0f) {
                        ImGui::SameLine(catX - rowPos.x);
                        renderBadgePill(desc->category.c_str(), tokens.surfaces.frameBg, tokens.borders.subtle, tokens.text.muted, 18.0f);
                    }
                }

                if (clicked) {
                    m_selectedIndex = static_cast<int>(i);
                    instantiateSelectedPlugin();
                    ImGui::PopID();
                    break;
                }

                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // Footer status bar
        ImGui::Spacing();
        ImGui::TextColored(tokens.text.muted.vec4, "[UP/DOWN] Navigate    [ENTER] Insert Plugin    [ESC] Dismiss");
        ImGui::SameLine(modalSize.x - 170.0f);
        ImGui::TextColored(tokens.text.muted.vec4, "%zu plugins available", m_filteredItems.size());
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace praccy::ui
