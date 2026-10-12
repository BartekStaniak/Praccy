#include "export_daw_chain_modal.h"
#include "../design_tokens.h"
#include "../../state/daw_interop/universal_daw_interop_manager.h"
#include "imgui.h"
#include <windows.h>
#include <shobjidl.h>

namespace praccy::ui {

void ExportDawChainModal::render(state::SceneManager& sceneMgr, audio::GraphEngine& graph) {
    if (!m_isOpen) return;

    const auto& tokens = themeTokens();

    ImGui::OpenPopup("Export Chain as DAW Preset");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 320.0f), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, static_cast<ImVec4>(tokens.surfaces.popupBg));
    ImGui::PushStyleColor(ImGuiCol_Border, static_cast<ImVec4>(tokens.borders.focus));

    if (ImGui::BeginPopupModal("Export Chain as DAW Preset", &m_isOpen, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(tokens.text.primary.vec4, "UNIVERSAL DAW PRESET EXPORTER");
        ImGui::TextColored(tokens.text.secondary.vec4, "Export the active Praccy signal chain directly to foreign DAWs.");
        ImGui::Separator();
        ImGui::Spacing();

        // 1. Format Selection
        ImGui::TextColored(tokens.text.accent.vec4, "Target DAW Format:");
        const char* formats[] = {
            "Cockos Reaper (.RfxChain)",
            "Ableton Live Audio Effect Rack (.adg)",
            "Open DAWproject 1.0 (.dawproject)",
            "Steinberg VST3 Preset (.vstpreset)"
        };
        ImGui::Combo("##FormatCombo", &m_targetFormatIdx, formats, IM_ARRAYSIZE(formats));

        ImGui::Spacing();

        // 2. Filename input
        ImGui::TextColored(tokens.text.accent.vec4, "Preset Filename:");
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_exportFilename.c_str());
        if (ImGui::InputText("##PresetName", nameBuf, sizeof(nameBuf))) {
            m_exportFilename = nameBuf;
        }

        // Default export directory if not set
        if (m_exportDirectory.empty()) {
            const char* userProfile = std::getenv("USERPROFILE");
            if (userProfile) {
                m_exportDirectory = (std::filesystem::path(userProfile) / "Documents" / "Praccy" / "Presets").string();
            } else {
                m_exportDirectory = "Presets";
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(tokens.text.secondary.vec4, "Export Folder: %s", m_exportDirectory.c_str());

        if (ImGui::Button("Browse Folder...")) {
            // Native folder picker
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            IFileOpenDialog* pFileOpen = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen)))) {
                FILEOPENDIALOGOPTIONS opt;
                if (SUCCEEDED(pFileOpen->GetOptions(&opt))) {
                    pFileOpen->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                }
                if (SUCCEEDED(pFileOpen->Show(nullptr))) {
                    IShellItem* pItem = nullptr;
                    if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                        PWSTR pszFilePath = nullptr;
                        if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                            int req = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, nullptr, 0, nullptr, nullptr);
                            std::string sel(req - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, sel.data(), req, nullptr, nullptr);
                            m_exportDirectory = sel;
                            CoTaskMemFree(pszFilePath);
                        }
                        pItem->Release();
                    }
                }
                pFileOpen->Release();
            }
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }

        ImGui::Spacing();
        if (!m_statusText.empty()) {
            ImGui::TextColored(tokens.signal.active.vec4, "%s", m_statusText.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 3. Action Buttons
        if (ImGui::Button("Export", ImVec2(120.0f, 28.0f))) {
            std::error_code ec;
            std::filesystem::create_directories(m_exportDirectory, ec);

            const char* ext = ".RfxChain";
            if (m_targetFormatIdx == 1) ext = ".adg";
            else if (m_targetFormatIdx == 2) ext = ".dawproject";
            else if (m_targetFormatIdx == 3) ext = ".vstpreset";

            std::filesystem::path outPath = std::filesystem::path(m_exportDirectory) / (m_exportFilename + ext);
            state::ScenePreset scene = sceneMgr.captureGraphSnapshot(graph, m_exportFilename);

            std::string statusMsg;
            if (state::UniversalDawInteropManager::instance().exportScene(scene, outPath, &statusMsg)) {
                m_statusText = "Successfully exported to " + outPath.filename().string();
                m_isOpen = false;
                ImGui::CloseCurrentPopup();
            } else {
                m_statusText = "Export failed: " + statusMsg;
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 28.0f))) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

} // namespace praccy::ui
