#include "ServiceProxyInternal.h"
#include "IpcProtocol.h"
#include "Logger.h"
#include <fstream>

namespace App {

void ServiceProxy::SaveConfig() {
    if (!IsLiveControlAllowed()) return;
    // 1. 生成服务层的配置段（[Service]段）
    std::string serviceBlock = "[Service]\n";
    serviceBlock += "mode=" + std::string(m_srvModeFull ? "full" : "touch_only") + "\n";
    serviceBlock += "auto_mode=" + std::string(m_srvAutoMode ? "1" : "0") + "\n";
    serviceBlock += "stylus_vhf_enabled=" + std::string(m_srvStylusVhfEnabled ? "1" : "0") + "\n";

    // 2. 将全量配置写回
    std::ofstream out(kConfigPath);
    if (!out.is_open()) return;

    out << serviceBlock << "\n";
    // TouchPipeline (unified section)
    out << "[TouchPipeline]\n";
    m_pipeline.SaveConfig(out);
    out << "\n";
    // Write stylus pipeline config
    out << "[StylusPipeline]\n";
    m_stylusPipeline.SaveConfig(out);
    out << "\n";
    out.close();
    // Notify Service to reload from config.ini
    m_configDirty.SetDirty();
    m_client.ReloadConfig();
    LOG_INFO("App", __func__, "IPC", "Config saved and Service notified to reload.");
}

void ServiceProxy::LoadConfig() {
    std::ifstream in(kConfigPath);
    if (!in.is_open()) return;
    std::string line, section;
    while (std::getline(in, line)) {
        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = TrimCopy(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            continue;
        }

        std::string key;
        std::string value;
        if (!ParseIniKeyValue(trimmed, key, value)) continue;

        if (section == "Service") {
            if (key == "mode") m_srvModeFull = (value == "full");
            else if (key == "auto_mode") m_srvAutoMode = (value == "1" || value == "true");
            else if (key == "stylus_vhf_enabled") m_srvStylusVhfEnabled = (value == "1" || value == "true");
        } else if (section == "TouchPipeline") {
            m_pipeline.LoadConfig(key, value);
        } else if (section == "StylusPipeline") {
            m_stylusPipeline.LoadConfig(key, value);
        } else if (IsLegacyTouchSection(section)) {
            const auto mappedKey = MapLegacyTouchKey(section, key);
            if (mappedKey.has_value()) {
                m_pipeline.LoadConfig(*mappedKey, value);
            }
        }
    }
}

void ServiceProxy::NotifyConfigDirty() {
    m_configDirty.SetDirty();
}

void ServiceProxy::SetSrvModeFull(bool full) {
    if (!IsLiveControlAllowed()) return;
    m_srvModeFull = full;
}

void ServiceProxy::SetSrvStylusVhfEnabled(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvStylusVhfEnabled = enabled;
}

void ServiceProxy::SetSrvAutoMode(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvAutoMode = enabled;
}

// ── MasterParser-only mode (local) ──
void ServiceProxy::SetMasterParserOnlyMode(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    // With new TouchPipeline, toggle individual module enables
    if (enabled && !m_masterParserOnly) {
        m_savedMasterOnly = true;
        // Disable all signal conditioning + processing beyond frame parse
        m_pipeline.m_baseline.m_enabled = false;
        m_pipeline.m_cmf.m_enabled = false;
        m_pipeline.m_gridIIR.m_enabled = false;
        m_pipeline.m_tracker.m_enabled = false;
        m_pipeline.m_coordFilter.m_enabled = false;
        m_pipeline.m_gesture.m_enabled = false;
    } else if (!enabled && m_masterParserOnly) {
        // Restore defaults
        m_pipeline.m_baseline.m_enabled = true;
        m_pipeline.m_cmf.m_enabled = true;
        m_pipeline.m_gridIIR.m_enabled = true;
        m_pipeline.m_tracker.m_enabled = true;
        m_pipeline.m_coordFilter.m_enabled = true;
        m_pipeline.m_gesture.m_enabled = true;
    }
    m_masterParserOnly = enabled;
}

} // namespace App
