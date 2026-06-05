#include "ServiceProxyInternal.h"
#include "Ipc/IpcProtocol.h"
#include "Logger.h"
#include "SolverBuildConfig.h"
#include "config/ConfigBinder.h"
#include "config/ConfigKeyMap.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace App {
namespace {

enum class ConfigServiceMode {
    Full,
    TouchOnly,
};

struct ConfigServiceMirrorState {
    ConfigServiceMode mode = ConfigServiceMode::Full;
    bool autoMode = true;
    bool stylusVhfEnabled = true;
    PenButtonMode penButtonMode = PenButtonMode::OemCustom;
    PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly;

    void registerBindings(Config::ConfigBinder& binder) {
        static const std::array<std::pair<ConfigServiceMode, std::string>, 2> kModeNames{{
            {ConfigServiceMode::Full, "full"},
            {ConfigServiceMode::TouchOnly, "touch_only"},
        }};
        static const std::array<std::pair<PenButtonMode, std::string>, 3> kPenButtonModeNames{{
            {PenButtonMode::OemCustom, "oem_custom"},
            {PenButtonMode::NativeBarrel, "native_barrel"},
            {PenButtonMode::NativeEraser, "native_eraser"},
        }};
        static const std::array<std::pair<PenButtonRoute, std::string>, 3> kPenButtonRouteNames{{
            {PenButtonRoute::VhfOnly, "vhf_only"},
            {PenButtonRoute::Win32Only, "win32_only"},
            {PenButtonRoute::VhfAndWin32, "vhf_and_win32"},
        }};

        binder.bindEnum("service.mode", &ConfigServiceMirrorState::mode, *this,
                        ConfigServiceMode::Full, std::span<const std::pair<ConfigServiceMode, std::string>>{kModeNames.data(), kModeNames.size()},
                        "Service operating mode (full | touch_only)");
        binder.bind("service.auto_mode", &ConfigServiceMirrorState::autoMode, *this,
                    true, {}, "Auto-select service mode");
        binder.bind("service.stylus_vhf_enabled", &ConfigServiceMirrorState::stylusVhfEnabled, *this,
                    true, {}, "Enable stylus VHF output");
        binder.bindEnum("service.pen_button_mode", &ConfigServiceMirrorState::penButtonMode, *this,
                        PenButtonMode::OemCustom, std::span<const std::pair<PenButtonMode, std::string>>{kPenButtonModeNames.data(), kPenButtonModeNames.size()},
                        "Pen button mode (oem_custom | native_barrel | native_eraser)");
        binder.bindEnum("service.pen_button_route", &ConfigServiceMirrorState::penButtonRoute, *this,
                        PenButtonRoute::VhfOnly, std::span<const std::pair<PenButtonRoute, std::string>>{kPenButtonRouteNames.data(), kPenButtonRouteNames.size()},
                        "Pen button injection route (vhf_only | win32_only | vhf_and_win32)");
    }
};

PenButtonMode ParsePenButtonModeName(const std::string& value) {
    if (value == "native_barrel") return PenButtonMode::NativeBarrel;
    if (value == "native_eraser") return PenButtonMode::NativeEraser;
    return PenButtonMode::OemCustom;
}

PenButtonRoute ParsePenButtonRouteName(const std::string& value) {
    if (value == "win32_only") return PenButtonRoute::Win32Only;
    if (value == "vhf_and_win32") return PenButtonRoute::VhfAndWin32;
    return PenButtonRoute::VhfOnly;
}

} // namespace

void ServiceProxy::SaveConfig() {
#if !EGOTOUCH_CONFIG_ENABLED
    return;  // Release: no config file I/O
#endif
    ApplyConfigStoreToLocalRuntime();
    if (!IsLiveControlAllowed()) return;

    const bool serviceConnected = m_client.IsConnected();
    const bool requestedSrvModeFull = m_srvDesiredModeFull.load(std::memory_order_relaxed);
    const bool requestedSrvAutoMode = m_srvAutoMode.load(std::memory_order_relaxed);
    const bool requestedSrvStylusVhfEnabled = m_srvStylusVhfEnabled.load(std::memory_order_relaxed);
    const PenButtonMode requestedPenButtonMode = m_srvPenButtonMode.load(std::memory_order_relaxed);
    const PenButtonRoute requestedPenButtonRoute = m_srvPenButtonRoute.load(std::memory_order_relaxed);

    std::string serviceSection = BuildServiceConfigSection(
        requestedSrvModeFull,
        requestedSrvAutoMode,
        requestedSrvStylusVhfEnabled,
        requestedPenButtonMode,
        requestedPenButtonRoute);

    Ipc::ConfigMutationResultWire patchSummary{};
    bool havePatchSummary = false;
    bool skipLocalServiceSection = false;
    bool servicePersistSucceeded = true;

    // Thin-client primary path: Service is authoritative for [Service] config.
    if (serviceConnected) {
        Ipc::ApplyConfigPatchRequestWire patch{};
        patch.fieldMask = Ipc::ToBits(Ipc::ServiceConfigFieldWire::Mode) |
                          Ipc::ToBits(Ipc::ServiceConfigFieldWire::AutoMode) |
                          Ipc::ToBits(Ipc::ServiceConfigFieldWire::StylusVhfEnabled) |
                          Ipc::ToBits(Ipc::ServiceConfigFieldWire::PenButtonMode) |
                          Ipc::ToBits(Ipc::ServiceConfigFieldWire::PenButtonRoute);
        patch.desiredMode = static_cast<uint8_t>(requestedSrvModeFull ? Ipc::ServiceModeWire::Full
                                                                      : Ipc::ServiceModeWire::TouchOnly);
        patch.autoMode = requestedSrvAutoMode ? 1 : 0;
        patch.stylusVhfEnabled = requestedSrvStylusVhfEnabled ? 1 : 0;
        patch.penButtonMode = static_cast<uint8_t>(requestedPenButtonMode);
        patch.penButtonRoute = static_cast<uint8_t>(requestedPenButtonRoute);

        const auto applyResp = m_client.ApplyConfigPatch(patch);
        if (!applyResp.success) {
            LOG_WARN("App", __func__, "IPC", "ApplyConfigPatch failed with status={}", static_cast<unsigned int>(applyResp.status));
            return;
        }
        if (applyResp.dataLen >= sizeof(patchSummary)) {
            std::memcpy(&patchSummary, applyResp.data, sizeof(patchSummary));
            havePatchSummary = true;
        }

        const auto persistResp = m_client.PersistConfig();
        if (!persistResp.success) {
            servicePersistSucceeded = false;
            LOG_WARN("App", __func__, "IPC", "ApplyConfigPatch succeeded but PersistConfig failed with status={}", static_cast<unsigned int>(persistResp.status));
            // Service owns [Service] config in connected mode. If persist fails,
            // preserve any existing local [Service] instead of writing client-side state.
            skipLocalServiceSection = true;
        }

        // Refresh mirrors from canonical snapshot (desired vs active semantics).
        const auto snapshotResp = m_client.GetConfigSnapshot();
        if (snapshotResp.success && snapshotResp.dataLen >= sizeof(Ipc::ConfigSnapshotWire)) {
            Ipc::ConfigSnapshotWire snapshot{};
            std::memcpy(&snapshot, snapshotResp.data, sizeof(snapshot));
            const bool desiredFull = snapshot.desiredMode == static_cast<uint8_t>(Ipc::ServiceModeWire::Full);
            const bool activeFull = snapshot.activeMode == static_cast<uint8_t>(Ipc::ServiceModeWire::Full);
            const bool autoMode = snapshot.autoMode != 0;
            const bool stylusVhfEnabled = snapshot.stylusVhfEnabled != 0;
            const auto penButtonMode = static_cast<PenButtonMode>(snapshot.penButtonMode);
            const auto penButtonRoute = static_cast<PenButtonRoute>(snapshot.penButtonRoute);

            m_srvDesiredModeFull.store(desiredFull, std::memory_order_relaxed);
            m_srvActiveModeFull.store(activeFull, std::memory_order_relaxed);
            m_srvAutoMode.store(autoMode, std::memory_order_relaxed);
            m_srvStylusVhfEnabled.store(stylusVhfEnabled, std::memory_order_relaxed);
            m_srvPenButtonMode.store(penButtonMode, std::memory_order_relaxed);
            m_srvPenButtonRoute.store(penButtonRoute, std::memory_order_relaxed);

            serviceSection = BuildServiceConfigSection(
                desiredFull, autoMode, stylusVhfEnabled,
                penButtonMode, penButtonRoute);
        } else {
            LOG_WARN("App", __func__, "IPC", "GetConfigSnapshot failed after patch/persist; skip local [Service] overwrite in connected mode.");
            skipLocalServiceSection = true;
        }
    }

    // Compatibility path: persist local pipeline sections for existing load/reload behavior.
    std::ifstream in(kConfigPath, std::ios::binary);
    std::string existingText;
    if (in.is_open()) {
        existingText.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
        in.close();
    }

    const TouchPipelineModuleEnableState* persistedModuleState = nullptr;
    if (m_masterParserOnly.load(std::memory_order_relaxed) && m_masterParserOnlySnapshot.has_value()) {
        persistedModuleState = &*m_masterParserOnlySnapshot;
    }

    const std::string mergedConfig = MergeServiceProxyConfigSections(
        existingText,
        skipLocalServiceSection ? std::string_view{} : std::string_view(serviceSection),
        BuildTouchPipelineConfigSection(m_pipeline, persistedModuleState),
        BuildStylusPipelineConfigSection(m_stylusPipeline),
        skipLocalServiceSection);

    const std::string tempPath = kConfigPath + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out << mergedConfig;
        out.flush();
        if (!out.good()) return;
    }

    const std::wstring configPathWide(kConfigPath.begin(), kConfigPath.end());
    const std::wstring tempPathWide(tempPath.begin(), tempPath.end());
    if (!MoveFileExW(tempPathWide.c_str(), configPathWide.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        DeleteFileW(tempPathWide.c_str());
        LOG_WARN("App", __func__, "Config", "Failed to replace config file: {}", static_cast<unsigned long>(err));
        return;
    }

    if (!serviceConnected) {
        LOG_INFO("App", __func__, "Config", "Config saved locally while service is disconnected.");
        return;
    }

    // Legacy compatibility: pipeline/stylus settings still apply via ReloadConfig.
    const auto reloadResp = m_client.ReloadConfig();
    if (!reloadResp.success) {
        LOG_WARN("App", __func__, "IPC", "Config patched/persisted and file updated, but ReloadConfig IPC failed with status={}", static_cast<unsigned int>(reloadResp.status));
    }

    Ipc::ReloadConfigSummaryWire reloadSummary{};
    if (reloadResp.success && reloadResp.dataLen >= sizeof(reloadSummary)) {
        std::memcpy(&reloadSummary, reloadResp.data, sizeof(reloadSummary));
    }

    Ipc::ConfigMutationResultWire summary{};
    if (havePatchSummary) {
        summary = patchSummary;
    } else {
        summary.changedFields = reloadSummary.changedFields;
        summary.appliedFields = reloadSummary.appliedFields;
        summary.restartRequiredFields = reloadSummary.restartRequiredFields;
    }

    const char* transactionState = servicePersistSucceeded ? "Config patched/persisted" : "Config patched but service persist failed";
    if (summary.restartRequiredFields != 0u || !servicePersistSucceeded) {
        LOG_WARN("App", __func__, "IPC",
                 "{}: changed=0x{:02X} applied_now=0x{:02X} restart_required=0x{:02X}.",
                 transactionState,
                 static_cast<unsigned int>(summary.changedFields),
                 static_cast<unsigned int>(summary.appliedFields),
                 static_cast<unsigned int>(summary.restartRequiredFields));
    } else {
        LOG_INFO("App", __func__, "IPC",
                 "{}: changed=0x{:02X} applied_now=0x{:02X} restart_required=0x{:02X}.",
                 transactionState,
                 static_cast<unsigned int>(summary.changedFields),
                 static_cast<unsigned int>(summary.appliedFields),
                 static_cast<unsigned int>(summary.restartRequiredFields));
    }
}

void ServiceProxy::LoadConfig() {
#if !EGOTOUCH_CONFIG_ENABLED
    return;  // Release: no config file I/O
#endif
    bool loadedServiceFromSnapshot = false;
    if (m_client.IsConnected()) {
        const auto resp = m_client.GetConfigSnapshot();
        if (resp.success && resp.dataLen >= sizeof(Ipc::ConfigSnapshotWire)) {
            Ipc::ConfigSnapshotWire snapshot{};
            std::memcpy(&snapshot, resp.data, sizeof(snapshot));
            m_srvDesiredModeFull.store(
                snapshot.desiredMode == static_cast<uint8_t>(Ipc::ServiceModeWire::Full),
                std::memory_order_relaxed);
            m_srvActiveModeFull.store(
                snapshot.activeMode == static_cast<uint8_t>(Ipc::ServiceModeWire::Full),
                std::memory_order_relaxed);
            m_srvAutoMode.store(snapshot.autoMode != 0, std::memory_order_relaxed);
            m_srvStylusVhfEnabled.store(snapshot.stylusVhfEnabled != 0, std::memory_order_relaxed);
            m_srvPenButtonMode.store(
                static_cast<PenButtonMode>(snapshot.penButtonMode),
                std::memory_order_relaxed);
            m_srvPenButtonRoute.store(
                static_cast<PenButtonRoute>(snapshot.penButtonRoute),
                std::memory_order_relaxed);
            loadedServiceFromSnapshot = true;
        }
    }

    std::ifstream in(kConfigPath);
    if (in.is_open()) {
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
                if (!loadedServiceFromSnapshot) {
                    if (key == "mode") {
                        const bool desiredFull = (value == "full");
                        m_srvDesiredModeFull.store(desiredFull, std::memory_order_relaxed);
                        // Offline fallback has no runtime distinction; mirror desired->active.
                        m_srvActiveModeFull.store(desiredFull, std::memory_order_relaxed);
                    } else if (key == "auto_mode") {
                        m_srvAutoMode.store(ParseServiceBool(value), std::memory_order_relaxed);
                    } else if (key == "stylus_vhf_enabled") {
                        m_srvStylusVhfEnabled.store(ParseServiceBool(value), std::memory_order_relaxed);
                    } else if (key == "pen_button_mode") {
                        int ival = std::atoi(value.c_str());
                        m_srvPenButtonMode.store(
                            static_cast<PenButtonMode>(std::clamp(ival, 0, 2)),
                            std::memory_order_relaxed);
                    } else if (key == "pen_button_route") {
                        int ival = std::atoi(value.c_str());
                        m_srvPenButtonRoute.store(
                            static_cast<PenButtonRoute>(std::clamp(ival, 0, 2)),
                            std::memory_order_relaxed);
                    }
                }
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

    InitConfigSchema();
}

void ServiceProxy::InitConfigSchema() {
#if !EGOTOUCH_CONFIG_ENABLED
    return;
#endif
    Config::ConfigBinder binder;
    m_pipeline.registerBindings(binder);
    m_stylusPipeline.registerBindings(binder);

    ConfigServiceMirrorState serviceState{};
    serviceState.mode = m_srvDesiredModeFull.load(std::memory_order_relaxed)
        ? ConfigServiceMode::Full
        : ConfigServiceMode::TouchOnly;
    serviceState.autoMode = m_srvAutoMode.load(std::memory_order_relaxed);
    serviceState.stylusVhfEnabled = m_srvStylusVhfEnabled.load(std::memory_order_relaxed);
    serviceState.penButtonMode = m_srvPenButtonMode.load(std::memory_order_relaxed);
    serviceState.penButtonRoute = m_srvPenButtonRoute.load(std::memory_order_relaxed);
    serviceState.registerBindings(binder);

    try {
        m_configDefaults.loadFromYaml("config/default.yaml");
    } catch (...) {
        m_configDefaults = Config::ConfigStore{};
        binder.writeDefaults(m_configDefaults);
    }
    m_configStore = m_configDefaults;
    binder.writeCurrent(m_configStore);
    m_configSchema = Config::BuildMergedSchema(m_configDefaults, binder);
}

void ServiceProxy::ApplyConfigStoreToLocalRuntime() {
#if !EGOTOUCH_CONFIG_ENABLED
    return;
#endif
    Config::ConfigBinder binder;
    m_pipeline.registerBindings(binder);
    m_stylusPipeline.registerBindings(binder);
    binder.apply(m_configStore);

    const std::string mode = m_configStore.getOr<std::string>("service.mode", m_srvDesiredModeFull.load(std::memory_order_relaxed) ? "full" : "touch_only");
    const bool desiredFull = mode != "touch_only";
    m_srvDesiredModeFull.store(desiredFull, std::memory_order_relaxed);
    if (!m_client.IsConnected()) {
        m_srvActiveModeFull.store(desiredFull, std::memory_order_relaxed);
    }
    m_srvAutoMode.store(m_configStore.getOr<bool>("service.auto_mode", m_srvAutoMode.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    m_srvStylusVhfEnabled.store(m_configStore.getOr<bool>("service.stylus_vhf_enabled", m_srvStylusVhfEnabled.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    m_srvPenButtonMode.store(ParsePenButtonModeName(m_configStore.getOr<std::string>("service.pen_button_mode", "oem_custom")), std::memory_order_relaxed);
    m_srvPenButtonRoute.store(ParsePenButtonRouteName(m_configStore.getOr<std::string>("service.pen_button_route", "vhf_only")), std::memory_order_relaxed);
}

std::vector<std::string> ServiceProxy::GetConfigModuleTags() const {
    std::vector<std::string> tags;
    tags.reserve(m_configSchema.entries.size());
    for (const auto& entry : m_configSchema.entries) {
        if (!entry.moduleTag.empty()) {
            tags.push_back(entry.moduleTag);
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

void ServiceProxy::SetSrvModeFull(bool full) {
    if (!IsLiveControlAllowed()) return;
    m_srvDesiredModeFull.store(full, std::memory_order_relaxed);
    m_configStore.set<std::string>("service.mode", full ? "full" : "touch_only");
}

void ServiceProxy::SetSrvStylusVhfEnabled(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvStylusVhfEnabled.store(enabled, std::memory_order_relaxed);
    m_configStore.set<bool>("service.stylus_vhf_enabled", enabled);
}

void ServiceProxy::SetSrvAutoMode(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvAutoMode.store(enabled, std::memory_order_relaxed);
    m_configStore.set<bool>("service.auto_mode", enabled);
}

void ServiceProxy::SetPenButtonMode(PenButtonMode m) {
    if (!IsLiveControlAllowed()) return;
    m_srvPenButtonMode.store(m, std::memory_order_relaxed);
    switch (m) {
        case PenButtonMode::NativeBarrel:
            m_configStore.set<std::string>("service.pen_button_mode", "native_barrel");
            break;
        case PenButtonMode::NativeEraser:
            m_configStore.set<std::string>("service.pen_button_mode", "native_eraser");
            break;
        case PenButtonMode::OemCustom:
        default:
            m_configStore.set<std::string>("service.pen_button_mode", "oem_custom");
            break;
    }
}

void ServiceProxy::SetPenButtonRoute(PenButtonRoute r) {
    if (!IsLiveControlAllowed()) return;
    m_srvPenButtonRoute.store(r, std::memory_order_relaxed);
    switch (r) {
        case PenButtonRoute::Win32Only:
            m_configStore.set<std::string>("service.pen_button_route", "win32_only");
            break;
        case PenButtonRoute::VhfAndWin32:
            m_configStore.set<std::string>("service.pen_button_route", "vhf_and_win32");
            break;
        case PenButtonRoute::VhfOnly:
        default:
            m_configStore.set<std::string>("service.pen_button_route", "vhf_only");
            break;
    }
}

// ── MasterParser-only mode (local) ──
void ServiceProxy::SetMasterParserOnlyMode(bool enabled) {
    if (enabled == m_masterParserOnly.load(std::memory_order_relaxed)) return;

    if (IsConnected() && IsLiveControlAllowed()) {
        Ipc::IpcRequest req{};
        req.command = Ipc::IpcCommand::SetMasterParserOnly;
        req.param[0] = enabled ? 1 : 0;
        req.paramLen = 1;
        m_client.Send(req);
    }

    if (enabled) {
        m_masterParserOnlySnapshot = CaptureTouchPipelineModuleEnableState(m_pipeline);
        TouchPipelineModuleEnableState parserOnlyState = *m_masterParserOnlySnapshot;
        parserOnlyState.baselineEnabled = false;
        parserOnlyState.cmfEnabled = false;
        parserOnlyState.gridIIREnabled = false;
        parserOnlyState.trackerEnabled = false;
        parserOnlyState.coordFilterEnabled = false;
        parserOnlyState.gestureEnabled = false;
        ApplyTouchPipelineModuleEnableState(m_pipeline, parserOnlyState);
    } else if (m_masterParserOnlySnapshot.has_value()) {
        ApplyTouchPipelineModuleEnableState(m_pipeline, *m_masterParserOnlySnapshot);
        m_masterParserOnlySnapshot.reset();
    }

    m_masterParserOnly.store(enabled, std::memory_order_relaxed);
}

} // namespace App
