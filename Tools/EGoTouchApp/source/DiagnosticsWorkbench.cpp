#include "DiagnosticsWorkbench.h"
#include "ServiceProxy.h"
#include "GuiLogSink.h"
#include "imgui_internal.h"
#include "imgui.h"
#include <string>

namespace App {

DiagnosticsWorkbench::DiagnosticsWorkbench(ServiceProxy* proxy) : m_proxy(proxy) {
}

DiagnosticsWorkbench::~DiagnosticsWorkbench() {
    // PenBridge is externally owned — no cleanup needed here
}

void DiagnosticsWorkbench::Render() {
    // Fetch current frame
    if (m_proxy) {
        m_proxy->UpdatePlayback();
    }
    if (m_autoRefresh && m_renderVisualization && m_proxy) {
        m_proxy->GetCurrentFrame(m_currentFrame);
    }

    // Hotkeys
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) m_fullscreen = !m_fullscreen;
    if (m_fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_fullscreen = false;

    // ── Fullscreen DockSpace ──
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockspaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockId, ImVec2(0, 0),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // First-frame: build programmatic dock layout
    if (!m_dockLayoutApplied) {
        SetupDockLayout(dockId);
        m_dockLayoutApplied = true;
    }
    ImGui::End();

    // ── Panels (each docked by name) ──
    DrawControlPanel();
    if (m_renderVisualization) DrawHeatmap();
    DrawLogPanel();
    DrawInspectorPanel();
    DrawStatusBar();
}

void DiagnosticsWorkbench::SetupDockLayout(ImGuiID dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    // Split: left 20% | remainder
    ImGuiID dockLeft, dockRemain;
    ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.20f,
                                &dockLeft, &dockRemain);
    // Split remainder: center 65% | right 35%
    ImGuiID dockCenter, dockRight;
    ImGui::DockBuilderSplitNode(dockRemain, ImGuiDir_Right, 0.35f,
                                &dockRight, &dockCenter);
    // Split center: heatmap top 70% | log bottom 30%
    ImGuiID dockHeatmap, dockLog;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.30f,
                                &dockLog, &dockHeatmap);
    // Split right: inspector top | status bottom
    ImGuiID dockInspector, dockStatusArea;
    ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.06f,
                                &dockStatusArea, &dockInspector);

    ImGui::DockBuilderDockWindow("Control Panel", dockLeft);
    ImGui::DockBuilderDockWindow("Heatmap", dockHeatmap);
    ImGui::DockBuilderDockWindow("Log", dockLog);
    ImGui::DockBuilderDockWindow("Inspector", dockInspector);
    ImGui::DockBuilderDockWindow("Status", dockStatusArea);
    ImGui::DockBuilderFinish(dockId);
}

void DiagnosticsWorkbench::DrawStatusBar() {
    ImGui::Begin("Status", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    if (m_proxy) {
        bool connected = m_proxy->IsConnected();
        if (connected) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f),
                               "● Connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.f),
                               "● Disconnected");
        }
        ImGui::SameLine(0, 20);
        ImGui::Text("Master FPS: %d", m_proxy->GetAcquisitionFps());
        ImGui::SameLine(0, 20);
        ImGui::Text("Slave FPS: %d", m_proxy->GetSlaveAcquisitionFps());
        ImGui::SameLine(0, 20);
        ImGui::Text("VHF: %s", m_proxy->IsVhfEnabled() ? "ON" : "OFF");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f),
                           "No ServiceProxy");
    }
    ImGui::End();
}

void DiagnosticsWorkbench::DrawLogPanel() {
    ImGui::Begin("Log");
    if (ImGui::BeginTabBar("LogTabs")) {
        if (ImGui::BeginTabItem("Console")) {
            if (ImGui::Button("Clear")) {
                Common::GuiLogSink::Instance()->Clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d lines)",
                (int)Common::GuiLogSink::Instance()->GetLines().size());
            ImGui::Separator();

            ImGui::BeginChild("LogScroll", ImVec2(0, 0), false,
                               ImGuiWindowFlags_HorizontalScrollbar);
            auto lines = Common::GuiLogSink::Instance()->GetLines();
            for (const auto& line : lines) {
                ImVec4 col = ImVec4(0.75f, 0.75f, 0.75f, 1.f);
                if (line.find("[error") != std::string::npos ||
                    line.find("[critical") != std::string::npos) {
                    col = ImVec4(1.0f, 0.35f, 0.35f, 1.f);
                } else if (line.find("[warning") != std::string::npos) {
                    col = ImVec4(1.0f, 0.8f, 0.3f, 1.f);
                } else if (line.find("[info") != std::string::npos) {
                    col = ImVec4(0.7f, 0.85f, 1.0f, 1.f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.f)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Master Suffix")) {
            DrawMasterSuffixTable();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Slave Suffix")) {
            DrawSlaveSuffixTable();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace App
