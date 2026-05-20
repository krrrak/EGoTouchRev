#include "DiagnosticsWorkbench.h"
#include "ServiceProxy.h"
#include "ConfigUIRenderer.h"
#include "GuiLogSink.h"
#include "SystemStateMonitor.h"
#include "imgui.h"
#include "Logger.h"
#include "StylusSolver/AsaTypes.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace App {

void DiagnosticsWorkbench::DrawInspectorPanel() {
    ImGui::Begin("Inspector");
    bool masterParserOnly = (m_proxy != nullptr) && m_proxy->IsMasterParserOnlyMode();
    // Level 1: category tabs
    if (ImGui::BeginTabBar("CategoryTabs")) {
        if (ImGui::BeginTabItem("Touch")) {
            if (m_proxy) {
                if (masterParserOnly) ImGui::BeginDisabled();
                auto schema = m_proxy->GetPipeline().GetConfigSchema();

                // Level 2: per-module sub-tabs
                if (ImGui::BeginTabBar("TouchModuleTabs")) {
                    static const char* moduleTabs[] = {
                        "Frame Parser",
                        "Signal Conditioning",
                        "Peak Detection",
                        "Zone & Contact",
                        "Palm Rejection",
                        "Tracking",
                        "Stylus Suppress",
                        "Coordinate Filter",
                        "Gesture",
                    };
                    for (const char* mod : moduleTabs) {
                        if (ImGui::BeginTabItem(mod)) {
                            ConfigUIRenderer::RenderConfigSchemaByModule(schema, mod);
                            // Show save button per tab
                            ImGui::Separator();
                            if (ImGui::Button("Save & Apply")) {
                                m_proxy->SaveConfig();
                            }
                            ImGui::EndTabItem();
                        }
                    }
                    // Coordinates table (diagnostic, not config)
                    if (ImGui::BeginTabItem("Coordinates")) {
                        DrawCoordinateTable();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                if (masterParserOnly) ImGui::EndDisabled();
            } else {
                ImGui::TextUnformatted("ServiceProxy unavailable.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stylus")) {
            DrawStylusControlPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("BT MCU")) {
            DrawBtMcuPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System Events")) {
            DrawSystemEventsPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DiagnosticsWorkbench::DrawTouchSolverPanel() {
    if (!m_proxy) {
        ImGui::TextUnformatted("ServiceProxy unavailable.");
        return;
    }

    const bool masterParserOnly = m_proxy->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    // Unified config schema from TouchPipeline
    auto schema = m_proxy->GetPipeline().GetConfigSchema();
    ConfigUIRenderer::RenderConfigSchema(schema, "Touch Solver (Feature Extraction)");

    if (masterParserOnly) {
        ImGui::EndDisabled();
    }
}

void DiagnosticsWorkbench::DrawTouchTrackingPanel() {
    if (!m_proxy) {
        ImGui::TextUnformatted("ServiceProxy unavailable.");
        return;
    }

    const bool masterParserOnly = m_proxy->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    // Unified config schema from TouchPipeline
    auto schema = m_proxy->GetPipeline().GetConfigSchema();
    ConfigUIRenderer::RenderConfigSchema(schema, "Tracking & Report");

    if (masterParserOnly) {
        ImGui::EndDisabled();
    }

    // --- FPS Stats ---
    ImGui::Separator();
    ImGui::TextUnformatted("Performance");
    if (m_proxy) {
        const int fps = m_proxy->GetAcquisitionFps();
        const int slaveFps = m_proxy->GetSlaveAcquisitionFps();
        auto fpsColor = [](int f) -> ImVec4 {
            if (f >= 100) return ImVec4(0.2f, 0.9f, 0.2f, 1.f);
            if (f >=  50) return ImVec4(1.0f, 0.8f, 0.0f, 1.f);
            return            ImVec4(1.0f, 0.3f, 0.3f, 1.f);
        };
        ImGui::TextColored(fpsColor(fps),       "Master Frame Rate: %d Hz", fps);
        ImGui::TextColored(fpsColor(slaveFps),   "Slave  Frame Rate: %d Hz", slaveFps);
    }
}

void DiagnosticsWorkbench::DrawStylusControlPanel() {
    if (!m_proxy) {
        ImGui::TextUnformatted("ServiceProxy unavailable.");
        return;
    }

    const bool masterParserOnly = m_proxy->IsMasterParserOnlyMode();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Master Parser Only is enabled.");
        ImGui::BeginDisabled();
    }

    // Global VHF Output Switch
    ImGui::TextUnformatted("Windows INK Output (VHF)");
    bool vhfStylus = m_proxy->IsSrvStylusVhfEnabled();
    if (ImGui::Checkbox("Enable Stylus Native Output", &vhfStylus)) {
        m_proxy->SetSrvStylusVhfEnabled(vhfStylus);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Pen Button Injection");
    {
        int curMode = static_cast<int>(m_proxy->GetPenButtonMode());
        const char* modeItems[] = {"OEM Custom", "Native Barrel", "Native Eraser"};
        if (ImGui::Combo("Button Mode", &curMode, modeItems, IM_ARRAYSIZE(modeItems))) {
            m_proxy->SetPenButtonMode(static_cast<PenButtonMode>(curMode));
        }

        int curRoute = static_cast<int>(m_proxy->GetPenButtonRoute());
        const char* routeItems[] = {"VHF Only", "Win32 Only", "VHF + Win32"};
        if (ImGui::Combo("Injection Route", &curRoute, routeItems, IM_ARRAYSIZE(routeItems))) {
            m_proxy->SetPenButtonRoute(static_cast<PenButtonRoute>(curRoute));
        }
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("StylusSubTabs")) {
        auto schema = m_proxy->GetStylusPipeline().GetConfigSchema();
        const auto& sd = m_currentFrame.stylus;
        const auto& diag = sd.debug.coord;
        const auto& point = sd.output.point;

        const float activeRows = std::max(1.0f, static_cast<float>(std::max(m_proxy->GetStylusPipeline().GetPacketSensorRows(), 1)) * static_cast<float>(Asa::kCoorUnit));
        const float activeCols = std::max(1.0f, static_cast<float>(std::max(m_proxy->GetStylusPipeline().GetPacketSensorCols(), 1)) * static_cast<float>(Asa::kCoorUnit));
        const float clampedY = std::clamp(point.y, 0.0f, activeRows);
        const float clampedX = std::clamp(point.x, 0.0f, activeCols);
        const uint16_t screenX = static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround((clampedY / activeRows) * 16000.0f)), 0, 65535));
        const uint16_t screenY = static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround((1.0f - (clampedX / activeCols)) * 25600.0f)), 0, 65535));
        const bool finalPointMismatch = diag.valid &&
            (std::abs(point.x - static_cast<float>(diag.finalDim1)) > 0.5f ||
             std::abs(point.y - static_cast<float>(diag.finalDim2)) > 0.5f);

        if (ImGui::BeginTabItem("Live Summary")) {
            ImGui::TextColored(sd.output.valid ? ImVec4(0.2f,0.9f,0.3f,1) : ImVec4(1.0f,0.45f,0.25f,1),
                "State: %s%s%s",
                sd.output.valid ? "Valid" : "Invalid",
                sd.output.inRange ? " | InRange" : "",
                sd.output.tipDown ? " | TipDown" : "");
            ImGui::Text("Pipeline Stage: %u", static_cast<unsigned int>(sd.output.pipelineStage));
            ImGui::Text("Pressure: final=%u  raw=%u  mapped=%u  real=%s  age=%u",
                sd.output.pressure,
                diag.rawPressure,
                diag.mappedPressure,
                diag.pressureIsReal ? "yes" : "no",
                static_cast<unsigned int>(diag.predictedAgeFrames));
            ImGui::Text("Signal: peak=%u  tx1=%u  tx2=%u", diag.peakSignal, sd.interop.signalX, sd.interop.signalY);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f,0.85f,1.0f,1.f), "Output Coordinate");
            ImGui::Text("Sensor: X=%.1f / %.1f  Y=%.1f / %.1f", point.x, activeCols, point.y, activeRows);
            ImGui::Text("Report: X=%u  Y=%u", static_cast<unsigned int>(point.reportX), static_cast<unsigned int>(point.reportY));
            ImGui::Text("Screen: X=%u / 16000  Y=%u / 25600", static_cast<unsigned int>(screenX), static_cast<unsigned int>(screenY));
            if (finalPointMismatch) {
                ImGui::TextColored(ImVec4(1.0f,0.35f,0.25f,1),
                    "Final coordinate and output point differ: final=(%d,%d) point=(%.1f,%.1f)",
                    diag.finalDim1, diag.finalDim2, point.x, point.y);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Coordinate Pipeline")) {
            ImGui::TextColored(ImVec4(0.4f,0.85f,1.0f,1.f), "Coordinate Stages (dim1=X/Col, dim2=Y/Row)");
            ImGui::Text("Anchor: row=%u  col=%u", diag.anchorRow, diag.anchorCol);
            ImGui::Text("Local:  dim1=%d  dim2=%d", diag.localCoorDim1, diag.localCoorDim2);
            ImGui::Text("Raw:    dim1=%d  dim2=%d", diag.rawDim1, diag.rawDim2);
            ImGui::Text("3PtAvg: dim1=%d  dim2=%d", diag.avg3PtDim1, diag.avg3PtDim2);
            ImGui::Text("Final:  dim1=%d  dim2=%d", diag.finalDim1, diag.finalDim2);
            ImGui::Text("Point:  x=%.1f  y=%.1f", point.x, point.y);
            if (finalPointMismatch) {
                ImGui::TextColored(ImVec4(1.0f,0.35f,0.25f,1), "Final/Point mismatch: downstream finalCoor changes may not be reflected in output.point.");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f,0.85f,0.4f,1.f), "Post Filters");
            ImGui::Text("Speed: instant=%.1f  short=%.1f  full=%.1f", diag.speedInstant, diag.speedShortAvg, diag.speedFullAvg);
            ImGui::Text("IIR: coef=%.0f", diag.iirCoef);
            ImGui::Text("Linear Filter: state=%u", static_cast<unsigned int>(diag.linearFilterState));
            ImGui::Text("CoorRevise: %s  delta=(%.1f, %.1f)",
                diag.coorReviserActive ? "on" : "off",
                diag.coorRevDeltaX,
                diag.coorRevDeltaY);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Tilt / Edge")) {
            ImGui::TextColored(ImVec4(0.8f,0.6f,1.0f,1.f), "Tilt");
            ImGui::Text("TX1/TX2 diff: filtered=(%.1f, %.1f)  raw=(%d, %d)",
                diag.tiltDiffX, diag.tiltDiffY, diag.tiltRawDiffDim1, diag.tiltRawDiffDim2);
            ImGui::Text("Tilt angle: pre=(%d, %d)  report=(%d, %d)",
                static_cast<int>(diag.preTiltDim1),
                static_cast<int>(diag.preTiltDim2),
                static_cast<int>(diag.reportTiltDim1),
                static_cast<int>(diag.reportTiltDim2));
            ImGui::Text("Signal ratio TX2/TX1: %u%%  lenLimit=%u",
                static_cast<unsigned int>(diag.signalRatio),
                static_cast<unsigned int>(diag.tiltLenLimit));
            if (diag.tiltAnomalyDamped) {
                ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1), "Tilt anomaly damping active");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f,0.85f,0.4f,1.f), "Edge / Signal Gates");
            ImGui::Text("Mode: %s%s", diag.isHover ? "Hover" : "Write", diag.isEdge ? " + Edge" : "");
            ImGui::Text("Edge flags: dim1=%s  dim2=%s", diag.dim1Edge ? "yes" : "no", diag.dim2Edge ? "yes" : "no");
            ImGui::Text("Peak: tx1=%u sum3x3=%u  tx2=%u sum3x3=%u tx2Valid=%s",
                diag.tx1PeakValue,
                diag.tx1Sum3x3,
                diag.tx2PeakValue,
                diag.tx2Sum3x3,
                diag.tx2Valid ? "yes" : "no");
            ImGui::Text("BT press suppress: %s", diag.btPressSuppressActive ? "active" : "inactive");
            ImGui::Text("Edge signal low latch: %s", diag.edgeSignalTooLowLatched ? "active" : "inactive");
            ImGui::Text("Fake pressure decrease: %s  framesLeft=%u",
                diag.fakePressureDecreaseActive ? "active" : "inactive",
                static_cast<unsigned int>(diag.fakePressureDecreaseFramesLeft));
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Config")) {
            if (ImGui::BeginTabBar("StylusConfigTabs")) {
                static const char* modules[] = {"Frame Parser", "Data Solve", "Pressure", "Coordinate", "Output"};
                for (const char* module : modules) {
                    if (ImGui::BeginTabItem(module)) {
                        ConfigUIRenderer::RenderConfigSchemaByModule(schema, module);
                        ImGui::Separator();
                        if (ImGui::Button("Save & Apply")) {
                            m_proxy->SaveConfig();
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Advanced Debug")) {
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.5f,1.f), "Linear Filter Internals");
            ImGui::Text("State=%u  lfState=%u  cos=%d  straightCount=%d  drag=%d",
                static_cast<unsigned int>(diag.linearFilterState),
                static_cast<unsigned int>(diag.lfStateMachine),
                diag.lfCos1000,
                diag.lfStraightBufCount,
                diag.lfDragApplied);
            ImGui::Text("Line fit: valid=%s  slope=%.4f  intercept=%.4f",
                diag.lfLineFitValid ? "yes" : "no",
                diag.lfLineFitSlopeA,
                diag.lfLineFitInterceptB);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f,0.5f,0.3f,1.f), "Pressure Internals");
            ImGui::Text("BT raw=%u  preIIR=%u  polySegment=%u  freqDebounceLeft=%u",
                diag.btRawPressure,
                diag.preIirPressure,
                static_cast<unsigned int>(diag.polySegment),
                static_cast<unsigned int>(diag.btFreqShiftDebounceFramesLeft));

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f,0.9f,1.0f,1.f), "Legacy / Derived Flags");
            ImGui::Text("lifecycle=%u  wasInking=%s  exitSmoothed=%s  cmf=%s  sigSuppress=%s  vhfPenState=0x%02X",
                static_cast<unsigned int>(diag.penLifecycle),
                diag.wasInking ? "yes" : "no",
                diag.exitSmoothed ? "yes" : "no",
                diag.cmfEnabled ? "yes" : "no",
                diag.sigSuppressActive ? "yes" : "no",
                static_cast<unsigned int>(diag.vhfPenState));

            ImGui::Separator();
            DrawDynamicDebugPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Stylus-Touch Interop settings are now accessible via the Touch Tracking tab's TrackTracker config.

    if (masterParserOnly) ImGui::EndDisabled();
}

void DiagnosticsWorkbench::DrawDynamicDebugPanel() {
    if (!m_proxy) {
        ImGui::TextUnformatted("ServiceProxy unavailable.");
        return;
    }

    const auto defs = m_proxy->GetDynamicDebugFields();
    ImGui::Text("Schema Version: %u", static_cast<unsigned int>(m_proxy->GetDynamicDebugSchemaVersion()));
    ImGui::SameLine();
    ImGui::Text("Schema Hash: %u", static_cast<unsigned int>(m_proxy->GetDynamicDebugSchemaHash()));

    if (defs.empty()) {
        ImGui::TextDisabled("No dynamic debug fields from service.");
        return;
    }

    std::vector<const DynamicDebugField*> ordered;
    ordered.reserve(defs.size());
    for (const auto& d : defs) ordered.push_back(&d);
    std::stable_sort(ordered.begin(), ordered.end(), [](const DynamicDebugField* a, const DynamicDebugField* b) {
        if (a->uiGroup != b->uiGroup) return a->uiGroup < b->uiGroup;
        if (a->uiOrder != b->uiOrder) return a->uiOrder < b->uiOrder;
        return a->fieldId < b->fieldId;
    });

    std::string currentGroup;
    for (const auto* def : ordered) {
        if (def->uiGroup != currentGroup) {
            if (!currentGroup.empty()) ImGui::Separator();
            currentGroup = def->uiGroup;
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "%s", currentGroup.empty() ? "Ungrouped" : currentGroup.c_str());
        }

        DynamicDebugValue v;
        const bool has = m_proxy->GetDynamicDebugValue(def->fieldId, v);
        if (!has || !v.valid) {
            ImGui::Text("%s: N/A", def->displayName.empty() ? def->key.c_str() : def->displayName.c_str());
            continue;
        }

        const char* label = def->displayName.empty() ? def->key.c_str() : def->displayName.c_str();
        switch (v.valueType) {
        case Ipc::DebugValueType::UInt32:
            ImGui::Text("%s: %u %s", label, static_cast<unsigned int>(v.rawValue & 0xFFFFFFFFu), def->unit.c_str());
            break;
        case Ipc::DebugValueType::Int32:
            ImGui::Text("%s: %d %s", label, static_cast<int32_t>(v.rawValue & 0xFFFFFFFFu), def->unit.c_str());
            break;
        case Ipc::DebugValueType::Float32: {
            uint32_t bits = static_cast<uint32_t>(v.rawValue & 0xFFFFFFFFu);
            float fv = 0.0f;
            std::memcpy(&fv, &bits, sizeof(fv));
            ImGui::Text("%s: %.4f %s", label, fv, def->unit.c_str());
            break;
        }
        case Ipc::DebugValueType::Bool:
            ImGui::Text("%s: %s", label, (v.rawValue & 0x1ull) ? "true" : "false");
            break;
        default:
            ImGui::Text("%s: <unknown>", label);
            break;
        }
    }
}

// ── BT MCU Panel (PenBridge Status — via IPC) ──
void DiagnosticsWorkbench::DrawBtMcuPanel() {
    ImGui::TextWrapped(
        "BT MCU dual-channel architecture: "
        "PenEventBridge (col00) handles auto-ACK + 0x7D01 echo. "
        "PenPressureReader (col01) decodes continuous 'U' reports.");
    ImGui::Separator();

    // ── Status + Pressure (via IPC GetPenBridgeStatus) ──
    auto ps = m_proxy ? m_proxy->GetPenBridgeStatus() : App::PenBridgeStatus{};

    ImGui::Text("EventBridge (col00):");
    ImGui::SameLine();
    if (ps.evtRunning)
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "RUNNING");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "STOPPED");

    ImGui::Text("PressReader (col01):");
    ImGui::SameLine();
    if (ps.pressRunning)
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "RUNNING");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "STOPPED");

    ImGui::Separator();
    ImGui::Text("Pressure Range Mode");
    int pressureMode = ps.pressureMode == 0 ? 0 : 1;
    const char* pressureModes[] = {"4096 raw12", "16382 raw14 / 4"};
    if (ImGui::Combo("##PenPressureMode", &pressureMode, pressureModes, 2) && m_proxy) {
        m_proxy->SetPenPressureMode(static_cast<uint8_t>(pressureMode));
    }

    ImGui::Separator();
    ImGui::Text("Live Pressure Data");
    ImGui::SameLine();
    ImGui::TextDisabled("(polled every ~500ms)");

    ImGui::Text("Report: 0x%02X (%c)  |  BT Freq: %d / %d",
                ps.reportType,
                ps.reportType >= 0x20 ? static_cast<char>(ps.reportType) : '?',
                ps.freq1, ps.freq2);

    for (int k = 0; k < 4; ++k) {
        ImGui::PushID(k);
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "P%d: %u raw:%u", k, ps.press[k], ps.rawPress[k]);
        const float denom = static_cast<float>(std::max<uint16_t>(ps.pressureMax, 1));
        ImGui::ProgressBar(static_cast<float>(ps.press[k]) / denom,
                           ImVec2(-1, 18), overlay);
        ImGui::PopID();
    }

    // ── MCU 相关日志 ──
    ImGui::Separator();
    ImGui::Text("MCU-related Service Logs");
    ImGui::SameLine();
    ImGui::TextDisabled("(refreshed every ~1s via IPC)");

    auto allLines = Common::GuiLogSink::Instance()->GetLines();
    ImGui::BeginChild("McuLogFilter", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : allLines) {
        if (line.find("PenEventBridge") != std::string::npos ||
            line.find("PenPressureReader") != std::string::npos ||
            line.find("PenBridge") != std::string::npos ||
            line.find("MCU") != std::string::npos ||
            line.find("PenEventCb") != std::string::npos ||
            line.find("PenConn") != std::string::npos ||
            line.find("PenFreq") != std::string::npos) {
            ImVec4 col(0.4f, 0.8f, 1.0f, 1.0f);
            if (line.find("ACK") != std::string::npos ||
                line.find("7D01") != std::string::npos)
                col = ImVec4(0.3f, 1.0f, 0.5f, 1.0f);
            if (line.find("[error") != std::string::npos ||
                line.find("failed") != std::string::npos)
                col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            if (line.find("[warn") != std::string::npos)
                col = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ── System Events Panel ──
void DiagnosticsWorkbench::DrawSystemEventsPanel() {
    ImGui::TextWrapped("Simulate system state events by signaling named events "
                       "that SystemStateMonitor is listening to.");
    ImGui::Separator();

#if defined(_DEBUG) || !defined(NDEBUG)
    struct EventInfo {
        const char* label;
        Host::SystemStateNamedEventId id;
        ImVec4 color;
    };
    const EventInfo items[] = {
        {"Display ON (Power)",     Host::SystemStateNamedEventId::MonitorPowerOn, {0.3f, 1.f, 0.3f, 1.f}},
        {"Display OFF (Power)",    Host::SystemStateNamedEventId::MonitorPowerOff, {1.f, 0.3f, 0.3f, 1.f}},
        {"Display ON (Console)",   Host::SystemStateNamedEventId::MonitorConsoleDisplayOn, {0.3f, 1.f, 0.3f, 1.f}},
        {"Display OFF (Console)",  Host::SystemStateNamedEventId::MonitorConsoleDisplayOff, {1.f, 0.3f, 0.3f, 1.f}},
        {"Lid ON",                 Host::SystemStateNamedEventId::MonitorLidOn, {0.3f, 0.8f, 1.f, 1.f}},
        {"Lid OFF",                Host::SystemStateNamedEventId::MonitorLidOff, {1.f, 0.6f, 0.2f, 1.f}},
        {"Shutdown",               Host::SystemStateNamedEventId::MonitorShutDown, {1.f, 0.2f, 0.2f, 1.f}},
        {"Resume Automatic",       Host::SystemStateNamedEventId::PbtApmResumeAutomatic, {0.5f, 1.f, 0.5f, 1.f}},
    };

    for (const auto& item : items) {
        ImGui::PushStyleColor(ImGuiCol_Button, item.color);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
        if (ImGui::Button(item.label, ImVec2(-1, 28))) {
            if (Host::SystemStateMonitor::SignalNamedEvent(item.id)) {
                LOG_INFO("App", __func__, "UI", "Signaled: {}", item.label);
            } else {
                LOG_WARN("App", __func__, "UI", "Failed to signal named event for {}", item.label);
            }
        }
        ImGui::PopStyleColor(2);
    }
#else
    ImGui::TextDisabled("Named-event signaling controls are available in debug/test builds only.");
#endif
}

} // namespace App
