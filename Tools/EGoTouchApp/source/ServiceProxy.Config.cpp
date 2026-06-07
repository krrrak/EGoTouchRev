#include "ServiceProxyInternal.h"
#include "Ipc/IpcProtocol.h"
#include "Logger.h"
#include "SolverBuildConfig.h"
#include "config/ConfigBinder.h"
#include "config/ConfigKeyMap.h"
#include "config/ConfigPath.h"
#include "config/ConfigTlv.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>


namespace App {

namespace {

Config::ConfigValue ToConfigValue(bool value) {
    return Config::ConfigValue(value);
}

Config::ConfigValue ToConfigValue(std::string_view value) {
    return Config::ConfigValue(std::string(value));
}

Config::ConfigValue ToConfigValue(const char* value) {
    return Config::ConfigValue(std::string(value));
}

const char* ToServiceModeConfig(bool full) {
    return full ? "full" : "touch_only";
}

std::string NormalizeConfigToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(value.begin(), value.end(), ' ', '_');
    std::replace(value.begin(), value.end(), '+', '_');
    std::replace(value.begin(), value.end(), '-', '_');
    while (value.find("__") != std::string::npos) {
        value.replace(value.find("__"), 2, "_");
    }
    return value;
}

const char* ToPenButtonModeConfig(PenButtonMode mode) {
    switch (mode) {
    case PenButtonMode::OemCustom: return "oem_custom";
    case PenButtonMode::NativeBarrel: return "native_barrel";
    case PenButtonMode::NativeEraser: return "native_eraser";
    }
    return "oem_custom";
}

const char* ToPenButtonRouteConfig(PenButtonRoute route) {
    switch (route) {
    case PenButtonRoute::VhfOnly: return "vhf_only";
    case PenButtonRoute::Win32Only: return "win32_only";
    case PenButtonRoute::VhfAndWin32: return "vhf_and_win32";
    }
    return "vhf_only";
}

std::optional<PenButtonMode> ParsePenButtonModeConfig(const Config::ConfigValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        const auto normalized = NormalizeConfigToken(*text);
        if (normalized == "oem_custom") return PenButtonMode::OemCustom;
        if (normalized == "native_barrel") return PenButtonMode::NativeBarrel;
        if (normalized == "native_eraser") return PenButtonMode::NativeEraser;
    }
    if (const auto* numeric = std::get_if<int32_t>(&value)) {
        if (*numeric >= 0 && *numeric <= 2) {
            return static_cast<PenButtonMode>(*numeric);
        }
    }
    return std::nullopt;
}

std::optional<PenButtonRoute> ParsePenButtonRouteConfig(const Config::ConfigValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        const auto normalized = NormalizeConfigToken(*text);
        if (normalized == "vhf_only") return PenButtonRoute::VhfOnly;
        if (normalized == "win32_only") return PenButtonRoute::Win32Only;
        if (normalized == "vhf_and_win32" || normalized == "vhf_win32") return PenButtonRoute::VhfAndWin32;
    }
    if (const auto* numeric = std::get_if<int32_t>(&value)) {
        if (*numeric >= 0 && *numeric <= 2) {
            return static_cast<PenButtonRoute>(*numeric);
        }
    }
    return std::nullopt;
}

void ApplyLegacyServiceModeMigration(Config::ConfigStore& target, const Config::ConfigStore& source) {
    if (source.has("service.mode") || !source.has("service.mode.full")) {
        return;
    }

    target.set<Config::ConfigValue>(
        "service.mode",
        ToConfigValue(source.getOr<bool>("service.mode.full", true) ? "full" : "touch_only"));
}

bool ServiceModeFullFromConfig(const Config::ConfigStore& store, bool fallback) {
    if (store.has("service.mode")) {
        const auto value = store.get<Config::ConfigValue>("service.mode");
        if (const auto* text = std::get_if<std::string>(&value)) {
            const auto normalized = NormalizeConfigToken(*text);
            if (normalized == "full") return true;
            if (normalized == "touch_only") return false;
        }
    }

    if (store.has("service.mode.full")) {
        return store.getOr<bool>("service.mode.full", fallback);
    }

    return fallback;
}

Config::ConfigValue ToConfigValue(PenButtonMode value) {
    return Config::ConfigValue(std::string(ToPenButtonModeConfig(value)));
}

Config::ConfigValue ToConfigValue(PenButtonRoute value) {
    return Config::ConfigValue(std::string(ToPenButtonRouteConfig(value)));
}

void SetValue(Config::ConfigStore& store, std::string_view path, Config::ConfigValue value) {
    store.set<Config::ConfigValue>(path, std::move(value));
}

Config::ConfigValueType ValueTypeFor(const Config::ConfigValue& value) {
    return std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return Config::ConfigValueType::Bool;
        } else if constexpr (std::is_same_v<T, int32_t>) {
            return Config::ConfigValueType::Int32;
        } else if constexpr (std::is_same_v<T, float>) {
            return Config::ConfigValueType::Float;
        } else {
            return Config::ConfigValueType::String;
        }
    }, value);
}

Config::ConfigTlvEntry BuildTlvEntry(Config::ConfigKeyId keyId, const Config::ConfigValue& value) {
    return Config::ConfigTlvEntry{
        .keyId = keyId,
        .valueType = ValueTypeFor(value),
        .stringValue = Config::toString(value),
    };
}

bool IsLivePatchableEntry(const Config::ConfigSchemaEntry& entry) {
    return entry.boundToRuntime &&
           (entry.runtimeBinding == Config::ConfigRuntimeBinding::LiveSetter ||
            entry.runtimeBinding == Config::ConfigRuntimeBinding::ManualLiveApply) &&
           Config::isLiveApplyTiming(entry.applyTiming);
}

bool IsV3PatchableEntry(const Config::ConfigSchemaEntry& entry) {
    return entry.boundToRuntime &&
           (entry.runtimeBinding == Config::ConfigRuntimeBinding::LiveSetter ||
            entry.runtimeBinding == Config::ConfigRuntimeBinding::ManualLiveApply) &&
           (Config::isLiveApplyTiming(entry.applyTiming) ||
            entry.applyTiming == Config::ConfigApplyTiming::RestartRequired);
}

std::optional<Config::ConfigValue> NormalizeSnapshotValueForSchema(const Config::ConfigSchemaEntry& schemaEntry,
                                                                 const Config::ConfigValue& value) {
    if (schemaEntry.uiType != Config::ConfigUiType::Enum && schemaEntry.enumMapping.empty()) {
        return value;
    }

    const auto* numeric = std::get_if<int32_t>(&value);
    if (numeric == nullptr) {
        return value;
    }

    const auto it = std::ranges::find_if(schemaEntry.enumMapping, [numeric](const auto& item) {
        return item.first == *numeric;
    });
    if (it == schemaEntry.enumMapping.end()) {
        LOG_WARN("App", __func__, "Config", "Skipping unmapped numeric enum snapshot value path={} value={}",
                 schemaEntry.yamlPath, *numeric);
        return std::nullopt;
    }

    return Config::ConfigValue(it->second);
}

bool IsKnownConfigV3MutationStatus(uint8_t status) {
    switch (static_cast<Ipc::ConfigV3MutationStatus>(status)) {
    case Ipc::ConfigV3MutationStatus::Ok:
    case Ipc::ConfigV3MutationStatus::NoChanges:
    case Ipc::ConfigV3MutationStatus::VersionMismatch:
    case Ipc::ConfigV3MutationStatus::Rejected:
    case Ipc::ConfigV3MutationStatus::PersistFailed:
        return true;
    }
    return false;
}

std::optional<Ipc::ConfigV3ApplyResultWire> DecodeConfigV3ApplyResult(const Ipc::IpcResponse& response) {
    if (!response.success || response.dataLen < sizeof(Ipc::ConfigV3ApplyResultWire)) {
        return std::nullopt;
    }

    Ipc::ConfigV3ApplyResultWire result{};
    std::memcpy(&result, response.data, sizeof(result));
    if (result.wireVersion != Ipc::kIpcProtocolVersion || !IsKnownConfigV3MutationStatus(result.status)) {
        return std::nullopt;
    }
    return result;
}

enum class ServiceModeSchema {
    Full,
    TouchOnly,
};

struct ServiceSchemaState {
    ServiceModeSchema mode = ServiceModeSchema::Full;
    bool autoMode = true;
    bool stylusVhfEnabled = true;
    PenButtonMode penButtonMode = PenButtonMode::OemCustom;
    PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly;
};

void RegisterServiceConfigSchemaBindings(Config::ConfigBinder& binder,
                                         ServiceSchemaState& state) {
    static const std::array<std::pair<ServiceModeSchema, std::string>, 2> kModeMapping{{
        {ServiceModeSchema::Full, "full"},
        {ServiceModeSchema::TouchOnly, "touch_only"},
    }};
    static const std::array<std::pair<PenButtonMode, std::string>, 3> kPenButtonModeMapping{{
        {PenButtonMode::OemCustom, "oem_custom"},
        {PenButtonMode::NativeBarrel, "native_barrel"},
        {PenButtonMode::NativeEraser, "native_eraser"},
    }};
    static const std::array<std::pair<PenButtonRoute, std::string>, 3> kPenButtonRouteMapping{{
        {PenButtonRoute::VhfOnly, "vhf_only"},
        {PenButtonRoute::Win32Only, "win32_only"},
        {PenButtonRoute::VhfAndWin32, "vhf_and_win32"},
    }};

    constexpr auto runtimeBinding = Config::ConfigRuntimeBinding::ManualLiveApply;
    binder.bindEnum("service.mode", &ServiceSchemaState::mode, state,
                    ServiceModeSchema::Full, std::span<const std::pair<ServiceModeSchema, std::string>>(kModeMapping), "Service runtime topology", runtimeBinding);
    binder.bind("service.auto_mode", &ServiceSchemaState::autoMode, state,
                true, {}, "Enable automatic runtime start/init", runtimeBinding);
    binder.bind("service.stylus_vhf_enabled", &ServiceSchemaState::stylusVhfEnabled, state,
                true, {}, "Enable stylus VHF output", runtimeBinding);
    binder.bindEnum("service.pen_button_mode", &ServiceSchemaState::penButtonMode, state,
                    PenButtonMode::OemCustom, std::span<const std::pair<PenButtonMode, std::string>>(kPenButtonModeMapping), "Pen button semantic mode", runtimeBinding);
    binder.bindEnum("service.pen_button_route", &ServiceSchemaState::penButtonRoute, state,
                    PenButtonRoute::VhfOnly, std::span<const std::pair<PenButtonRoute, std::string>>(kPenButtonRouteMapping), "Pen button injection route", runtimeBinding);
}

void PopulateServiceDefaults(Config::ConfigStore& store) {
    SetValue(store, "service.mode", ToConfigValue("full"));
    SetValue(store, "service.auto_mode", ToConfigValue(true));
    SetValue(store, "service.stylus_vhf_enabled", ToConfigValue(true));
    SetValue(store, "service.pen_button_mode", ToConfigValue(PenButtonMode::OemCustom));
    SetValue(store, "service.pen_button_route", ToConfigValue(PenButtonRoute::VhfOnly));
}

void PopulateServiceValues(Config::ConfigStore& store,
                           bool desiredModeFull,
                           bool autoMode,
                           bool stylusVhfEnabled,
                           PenButtonMode penButtonMode,
                           PenButtonRoute penButtonRoute) {
    SetValue(store, "service.mode", ToConfigValue(ToServiceModeConfig(desiredModeFull)));
    SetValue(store, "service.auto_mode", ToConfigValue(autoMode));
    SetValue(store, "service.stylus_vhf_enabled", ToConfigValue(stylusVhfEnabled));
    SetValue(store, "service.pen_button_mode", ToConfigValue(penButtonMode));
    SetValue(store, "service.pen_button_route", ToConfigValue(penButtonRoute));
}

void SyncServiceMirrorsFromStore(Config::ConfigStore& store,
                                 std::atomic<bool>& desiredModeFull,
                                 std::atomic<bool>& activeModeFull,
                                 std::atomic<bool>& autoMode,
                                 std::atomic<bool>& stylusVhfEnabled,
                                 std::atomic<PenButtonMode>& penButtonMode,
                                 std::atomic<PenButtonRoute>& penButtonRoute) {
    const bool full = ServiceModeFullFromConfig(store, desiredModeFull.load(std::memory_order_relaxed));
    desiredModeFull.store(full, std::memory_order_relaxed);
    activeModeFull.store(full, std::memory_order_relaxed);
    autoMode.store(store.getOr<bool>("service.auto_mode", autoMode.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    stylusVhfEnabled.store(store.getOr<bool>("service.stylus_vhf_enabled", stylusVhfEnabled.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    if (store.has("service.pen_button_mode")) {
        const auto value = store.get<Config::ConfigValue>("service.pen_button_mode");
        const auto parsed = ParsePenButtonModeConfig(value);
        penButtonMode.store(parsed.value_or(penButtonMode.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    if (store.has("service.pen_button_route")) {
        const auto value = store.get<Config::ConfigValue>("service.pen_button_route");
        const auto parsed = ParsePenButtonRouteConfig(value);
        penButtonRoute.store(parsed.value_or(penButtonRoute.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }

    PopulateServiceValues(
        store,
        desiredModeFull.load(std::memory_order_relaxed),
        autoMode.load(std::memory_order_relaxed),
        stylusVhfEnabled.load(std::memory_order_relaxed),
        penButtonMode.load(std::memory_order_relaxed),
        penButtonRoute.load(std::memory_order_relaxed));
}

} // namespace

void ServiceProxy::InitConfigSchema() {
    Config::ConfigBinder binder;
    ServiceSchemaState serviceSchemaState;
    RegisterServiceConfigSchemaBindings(binder, serviceSchemaState);
    m_pipeline.registerBindings(binder);
    m_stylusPipeline.registerBindings(binder);
    Config::registerRuntimeKeyMappings(binder);

    m_configDefaults = Config::ConfigStore{};
    PopulateServiceDefaults(m_configDefaults);
    binder.writeDefaults(m_configDefaults);

    m_configStore = Config::ConfigStore{};
    m_configStore.mergeFrom(m_configDefaults);
    binder.writeCurrent(m_configStore);

    const auto paths = Config::resolve();
    if (paths.has_value()) {
        try {
            Config::ConfigStore yamlStore;
            yamlStore.loadFromYaml(paths->defaultConfig);
            m_configStore.mergeFrom(yamlStore);
            ApplyLegacyServiceModeMigration(m_configStore, yamlStore);
            if (paths->overrideExists) {
                Config::ConfigStore overrides;
                overrides.loadFromYaml(paths->overrideConfig);
                m_configStore.mergeFrom(overrides);
                ApplyLegacyServiceModeMigration(m_configStore, overrides);
            }
            LOG_INFO("App", __func__, "Config", "Loaded app-local preview config: default='{}' overrides='{}' overrideExists={}",
                     paths->defaultConfig, paths->overrideConfig, paths->overrideExists);
        } catch (const std::exception& ex) {
            LOG_WARN("App", __func__, "Config", "Failed to load YAML config for app-local preview; using binder defaults: {}", ex.what());
        }
    } else {
        LOG_WARN("App", __func__, "Config", "config/default.yaml not found; using binder defaults for app-local preview.");
    }

    SyncServiceMirrorsFromStore(
        m_configStore,
        m_srvDesiredModeFull,
        m_srvActiveModeFull,
        m_srvAutoMode,
        m_srvStylusVhfEnabled,
        m_srvPenButtonMode,
        m_srvPenButtonRoute);
    ApplyConfigStoreToLocalRuntime();

    m_configSchema = Config::BuildMergedSchema(m_configDefaults, binder);
}

Config::ConfigSchemaSnapshot BuildServiceProxyConfigSchemaSnapshotForTest() {
    Config::ConfigBinder binder;
    ServiceSchemaState serviceSchemaState;
    Solvers::TouchPipeline touchPipeline;
    Solvers::StylusPipeline stylusPipeline;

    RegisterServiceConfigSchemaBindings(binder, serviceSchemaState);
    touchPipeline.registerBindings(binder);
    stylusPipeline.registerBindings(binder);
    Config::registerRuntimeKeyMappings(binder);

    Config::ConfigStore defaults;
    PopulateServiceDefaults(defaults);
    binder.writeDefaults(defaults);
    return Config::BuildMergedSchema(defaults, binder);
}

void ServiceProxy::ApplyConfigStoreToLocalRuntime() {
    m_pipeline.applyConfig(m_configStore);
    m_stylusPipeline.applyConfig(m_configStore);
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

void ServiceProxy::MarkConfigPathsDirty(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        m_dirtyConfigPaths.insert(path);
    }
}

ApplyConfigResult ServiceProxy::GetLastApplyConfigResult() const {
    ApplyConfigResult result{};
    result.status = m_lastApplyConfigStatus.load(std::memory_order_relaxed);
    result.liveApplied = m_lastApplyConfigLiveApplied.load(std::memory_order_relaxed);
    result.restartRequired = m_lastApplyConfigRestartRequired.load(std::memory_order_relaxed);
    result.persistAttempted = m_lastApplyConfigPersistAttempted.load(std::memory_order_relaxed);
    result.persisted = m_lastApplyConfigPersisted.load(std::memory_order_relaxed);
    result.unpersistedLiveChanges = m_hasUnpersistedLiveConfigChanges.load(std::memory_order_relaxed);
    result.persistStatus = static_cast<Ipc::IpcStatusCode>(m_lastApplyConfigPersistStatus.load(std::memory_order_relaxed));
    return result;
}

bool ServiceProxy::ApplyConfigStoreGlobally() {
    auto setApplyOutcome = [this](ApplyConfigStatus status, bool liveApplied, bool restartRequired) {
        m_lastApplyConfigStatus.store(status, std::memory_order_relaxed);
        m_lastApplyConfigLiveApplied.store(liveApplied, std::memory_order_relaxed);
        m_lastApplyConfigRestartRequired.store(restartRequired, std::memory_order_relaxed);
        m_lastApplyConfigPersistAttempted.store(false, std::memory_order_relaxed);
        m_lastApplyConfigPersisted.store(false, std::memory_order_relaxed);
        m_lastApplyConfigPersistStatus.store(static_cast<uint8_t>(Ipc::IpcStatusCode::InternalError), std::memory_order_relaxed);
    };
#if !EGOTOUCH_CONFIG_ENABLED
    setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
    return false;
#endif

    Config::ConfigPatchTlv patch{};
    size_t skippedKeys = 0;
    bool hasRestartRequiredEntry = false;
    std::vector<std::string> appliedPaths;
    std::vector<std::string> dirtyPaths(m_dirtyConfigPaths.begin(), m_dirtyConfigPaths.end());
    std::sort(dirtyPaths.begin(), dirtyPaths.end());
    for (const auto& path : dirtyPaths) {
        const auto schemaIt = std::find_if(m_configSchema.entries.begin(), m_configSchema.entries.end(),
            [&path](const Config::ConfigSchemaEntry& entry) { return entry.yamlPath == path; });
        if (schemaIt == m_configSchema.entries.end() ||
            schemaIt->keyId == Config::ConfigKeyId::MaxKeyId ||
            !m_configStore.has(path) || !IsV3PatchableEntry(*schemaIt)) {
            ++skippedKeys;
            continue;
        }

        const Config::ConfigValue value = m_configStore.get<Config::ConfigValue>(path);
        patch.entries.push_back(BuildTlvEntry(schemaIt->keyId, value));
        hasRestartRequiredEntry = hasRestartRequiredEntry ||
            schemaIt->applyTiming == Config::ConfigApplyTiming::RestartRequired;
        appliedPaths.push_back(path);
    }

    if (patch.entries.empty()) {
        setApplyOutcome(ApplyConfigStatus::NoChanges, false, false);
        LOG_INFO("App", __func__, "Config", "Global config apply produced no TLV entries; skippedKeys={}", skippedKeys);
        return true;
    }

    if (!IsLiveControlAllowed() || !IsConfigIpcConnectedForApply()) {
        ApplyConfigStoreToLocalRuntime();
        setApplyOutcome(hasRestartRequiredEntry ? ApplyConfigStatus::RestartRequired : ApplyConfigStatus::LiveApplied,
                        !hasRestartRequiredEntry,
                        hasRestartRequiredEntry);
        m_hasUnpersistedLiveConfigChanges.store(true, std::memory_order_relaxed);
        LOG_WARN("App", __func__, "IPC", "Global config apply used app-local fallback; live control unavailable or IPC disconnected.");
        return true;
    }

    std::vector<uint8_t> payload;
    try {
        payload = Config::serializePatch(patch);
    } catch (const std::exception& ex) {
        setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
        LOG_WARN("App", __func__, "Config", "Failed to serialize global config patch: {}", ex.what());
        return false;
    }

    if (payload.empty() || payload.size() > Ipc::kConfigPatchV3PayloadBytes) {
        setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
        LOG_WARN("App", __func__, "IPC", "Global config v3 patch too large: {} bytes", payload.size());
        return false;
    }

    if (m_configV3SnapshotSchemaVersion == 0 || m_configV3SnapshotVersion == 0) {
        if (!RefreshConfigSnapshotV3()) {
            setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
            LOG_WARN("App", __func__, "Config", "Cannot apply config v3 patch without a snapshot baseline.");
            return false;
        }
    }

    auto sendApply = [&]() {
        Ipc::ApplyConfigPatchV3RequestWire request{};
        request.baseSchemaVersion = m_configV3SnapshotSchemaVersion;
        request.baseSnapshotVersion = m_configV3SnapshotVersion;
        request.payloadBytes = static_cast<uint16_t>(payload.size());
        std::memcpy(request.bytes, payload.data(), payload.size());
        return SendApplyConfigPatchV3Request(request);
    };

    auto applyResp = sendApply();
    auto decodedApply = DecodeConfigV3ApplyResult(applyResp);
    if (!decodedApply.has_value()) {
        setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
        LOG_WARN("App", __func__, "IPC", "ApplyConfigPatchV3 failed with status={}; dirty paths retained for retry",
                 static_cast<unsigned int>(applyResp.status));
        return false;
    }
    Ipc::ConfigV3ApplyResultWire applyResult = *decodedApply;

    if (static_cast<Ipc::ConfigV3MutationStatus>(applyResult.status) == Ipc::ConfigV3MutationStatus::VersionMismatch) {
        if (!RefreshConfigSnapshotV3()) {
            setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
            LOG_WARN("App", __func__, "Config", "ApplyConfigPatchV3 version mismatch and snapshot refresh failed.");
            return false;
        }
        applyResp = sendApply();
        decodedApply = DecodeConfigV3ApplyResult(applyResp);
        if (!decodedApply.has_value()) {
            setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
            LOG_WARN("App", __func__, "IPC", "ApplyConfigPatchV3 retry failed with status={}", static_cast<unsigned int>(applyResp.status));
            return false;
        }
        applyResult = *decodedApply;
    }

    const auto mutationStatus = static_cast<Ipc::ConfigV3MutationStatus>(applyResult.status);
    if (mutationStatus != Ipc::ConfigV3MutationStatus::Ok &&
        mutationStatus != Ipc::ConfigV3MutationStatus::NoChanges) {
        setApplyOutcome(ApplyConfigStatus::LiveApplyFailed, false, false);
        LOG_WARN("App", __func__, "IPC", "ApplyConfigPatchV3 rejected status={} failedKeyId=0x{:04X}",
                 static_cast<unsigned int>(applyResult.status), applyResult.failedKeyId);
        return false;
    }

    const bool liveApplied = applyResult.appliedCount != 0;
    const bool restartRequired = applyResult.restartRequiredCount != 0;
    const auto applyStatus = mutationStatus == Ipc::ConfigV3MutationStatus::NoChanges
        ? ApplyConfigStatus::NoChanges
        : (restartRequired ? ApplyConfigStatus::RestartRequired : ApplyConfigStatus::LiveApplied);
    setApplyOutcome(applyStatus, liveApplied, restartRequired);

    const auto persistResp = SendPersistConfigV3Request();
    m_lastApplyConfigPersistAttempted.store(true, std::memory_order_relaxed);
    m_lastApplyConfigPersistStatus.store(static_cast<uint8_t>(persistResp.status), std::memory_order_relaxed);
    bool persisted = false;
    Ipc::PersistConfigV3ResponseWire persistResult{};
    if (persistResp.success && persistResp.dataLen >= sizeof(Ipc::PersistConfigV3ResponseWire)) {
        std::memcpy(&persistResult, persistResp.data, sizeof(persistResult));
        persisted = persistResult.wireVersion == Ipc::kIpcProtocolVersion &&
            static_cast<Ipc::ConfigV3MutationStatus>(persistResult.status) == Ipc::ConfigV3MutationStatus::Ok;
    }
    m_lastApplyConfigPersisted.store(persisted, std::memory_order_relaxed);
    if (!persisted) {
        m_hasUnpersistedLiveConfigChanges.store(true, std::memory_order_relaxed);
        LOG_WARN("App", __func__, "IPC", "PersistConfigV3 failed after apply with status={}; keeping dirty paths for retry",
                 static_cast<unsigned int>(persistResp.status));
    }

    RefreshConfigSnapshot();
    ApplyConfigStoreToLocalRuntime();
    if (persisted) {
        for (const auto& path : appliedPaths) {
            m_dirtyConfigPaths.erase(path);
        }
        if (m_dirtyConfigPaths.empty()) {
            m_hasUnpersistedLiveConfigChanges.store(false, std::memory_order_relaxed);
        }
    }
    LOG_INFO("App", __func__, "IPC", "Global config v3 applied entries={} skippedKeys={} payloadBytes={} liveApplied={} restartRequired={} persisted={} persistedCount={} skippedPersist={} unpersistedLiveChanges={}",
             patch.entries.size(), skippedKeys, payload.size(), liveApplied ? 1 : 0, restartRequired ? 1 : 0,
             persisted ? 1 : 0, persistResult.persistedCount, persistResult.skippedCount,
             m_hasUnpersistedLiveConfigChanges.load(std::memory_order_relaxed) ? 1 : 0);
    return true;
}

void ServiceProxy::SaveConfig() {
    (void)ApplyConfigStoreGlobally();
}

bool ServiceProxy::IsConfigIpcConnectedForApply() const {
#if defined(EGOTOUCH_APP_SERVICE_PROXY_TEST)
    if (m_configV3IpcTestConnected) {
        return true;
    }
#endif
    return m_client.IsConnected();
}

Ipc::IpcResponse ServiceProxy::SendApplyConfigPatchV3Request(const Ipc::ApplyConfigPatchV3RequestWire& request) {
#if defined(EGOTOUCH_APP_SERVICE_PROXY_TEST)
    if (m_configV3IpcTestConnected) {
        m_lastConfigV3ApplyRequestForTest = request;
        m_hasLastConfigV3ApplyRequestForTest = true;
        ++m_configV3ApplyRequestCountForTest;
        if (m_configV3IpcTestApplyResponseIndex < m_configV3IpcTestApplyResponses.size()) {
            return m_configV3IpcTestApplyResponses[m_configV3IpcTestApplyResponseIndex++];
        }
        return m_configV3IpcTestApplyResponse;
    }
#endif
    return m_client.ApplyConfigPatchV3(request);
}

Ipc::IpcResponse ServiceProxy::SendPersistConfigV3Request() {
#if defined(EGOTOUCH_APP_SERVICE_PROXY_TEST)
    if (m_configV3IpcTestConnected) {
        ++m_configV3PersistRequestCountForTest;
        return m_configV3IpcTestPersistResponse;
    }
#endif
    return m_client.PersistConfigV3();
}

#if defined(EGOTOUCH_APP_SERVICE_PROXY_TEST)
void ServiceProxy::SetConfigV3IpcTestResponses(bool connected,
                                               const Ipc::IpcResponse& applyResponse,
                                               const Ipc::IpcResponse& persistResponse) {
    SetConfigV3IpcTestResponses(connected, std::vector<Ipc::IpcResponse>{applyResponse}, persistResponse);
}

void ServiceProxy::SetConfigV3IpcTestResponses(bool connected,
                                               std::vector<Ipc::IpcResponse> applyResponses,
                                               const Ipc::IpcResponse& persistResponse,
                                               std::vector<uint8_t> snapshotBytes) {
    m_configV3IpcTestConnected = connected;
    m_configV3IpcTestApplyResponses = std::move(applyResponses);
    m_configV3IpcTestApplyResponseIndex = 0;
    m_configV3IpcTestApplyResponse = m_configV3IpcTestApplyResponses.empty()
        ? Ipc::IpcResponse{}
        : m_configV3IpcTestApplyResponses.back();
    m_configV3IpcTestPersistResponse = persistResponse;
    m_configV3IpcTestSnapshotBytes = std::move(snapshotBytes);
    m_hasLastConfigV3ApplyRequestForTest = false;
    m_lastConfigV3ApplyRequestForTest = Ipc::ApplyConfigPatchV3RequestWire{};
    m_configV3ApplyRequestCountForTest = 0;
    m_configV3PersistRequestCountForTest = 0;
}
#endif

uint32_t HashBytes(std::span<const uint8_t> bytes) noexcept {
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

std::optional<std::vector<uint8_t>> ServiceProxy::FetchConfigV3Bytes(Ipc::ConfigV3PayloadKind payloadKind) {
#if defined(EGOTOUCH_APP_SERVICE_PROXY_TEST)
    if (m_configV3IpcTestConnected &&
        payloadKind == Ipc::ConfigV3PayloadKind::Snapshot &&
        !m_configV3IpcTestSnapshotBytes.empty()) {
        return m_configV3IpcTestSnapshotBytes;
    }
#endif
    constexpr uint32_t kMaxBytes = 1024u * 1024u;
    const uint8_t rawKind = static_cast<uint8_t>(payloadKind);
    std::vector<uint8_t> bytes;
    uint32_t schemaVersion = 0;
    uint32_t snapshotVersion = 0;
    uint32_t totalBytes = 0;
    uint32_t checksum = 0;
    uint32_t offset = 0;

    do {
        Ipc::ConfigV3PageRequestWire request{};
        request.payloadKind = rawKind;
        request.schemaVersion = schemaVersion;
        request.snapshotVersion = snapshotVersion;
        request.offset = offset;
        request.maxBytes = Ipc::ConfigV3PageCapacityBytes();

        const auto resp = payloadKind == Ipc::ConfigV3PayloadKind::Catalog
            ? m_client.GetConfigCatalogV3Page(request)
            : m_client.GetConfigSnapshotV3Page(request);
        if (!resp.success || resp.dataLen < sizeof(Ipc::ConfigV3PageResponseHeaderWire)) {
            LOG_WARN("App", __func__, "Config", "Config v3 page request failed kind={} offset={} status={}", rawKind, offset, static_cast<unsigned int>(resp.status));
            return std::nullopt;
        }

        Ipc::ConfigV3PageResponseHeaderWire header{};
        std::memcpy(&header, resp.data, sizeof(header));
        if (!Ipc::IsValidConfigV3PageResponse(header, resp.dataLen) || header.payloadKind != rawKind || header.offset != offset) {
            LOG_WARN("App", __func__, "Config", "Invalid config v3 page header kind={} offset={}", rawKind, offset);
            return std::nullopt;
        }
        if (offset == 0) {
            if (header.totalBytes > kMaxBytes) {
                LOG_WARN("App", __func__, "Config", "Config v3 payload too large kind={} bytes={}", rawKind, header.totalBytes);
                return std::nullopt;
            }
            schemaVersion = header.schemaVersion;
            snapshotVersion = header.snapshotVersion;
            totalBytes = header.totalBytes;
            checksum = header.checksum;
            bytes.reserve(totalBytes);
        } else if (header.schemaVersion != schemaVersion || header.snapshotVersion != snapshotVersion ||
                   header.totalBytes != totalBytes || header.checksum != checksum) {
            LOG_WARN("App", __func__, "Config", "Config v3 page metadata changed kind={} offset={}", rawKind, offset);
            return std::nullopt;
        }

        const auto* page = resp.data + header.headerBytes;
        bytes.insert(bytes.end(), page, page + header.pageBytes);
        offset += header.pageBytes;
        if (header.pageBytes == 0 && offset < totalBytes) {
            LOG_WARN("App", __func__, "Config", "Config v3 page made no progress kind={} offset={}", rawKind, offset);
            return std::nullopt;
        }
    } while (offset < totalBytes);

    if (bytes.size() != totalBytes || HashBytes(std::span<const uint8_t>(bytes.data(), bytes.size())) != checksum) {
        LOG_WARN("App", __func__, "Config", "Config v3 payload checksum mismatch kind={} bytes={}", rawKind, bytes.size());
        return std::nullopt;
    }
    return bytes;
}

bool ServiceProxy::ApplyConfigV3CatalogBytes(const uint8_t* data, size_t size) {
    try {
        const auto payload = Config::deserializeConfigV3Catalog(data, size);
        Config::ConfigCatalog catalog(payload.entries);
        m_configSchema = Config::BuildSchemaSnapshot(catalog);
        m_configV3CatalogSchemaVersion = payload.schemaVersion;
        m_configV3CatalogSnapshotVersion = payload.snapshotVersion;
        for (const auto& entry : payload.entries) {
            if (!entry.path.empty()) {
                m_configDefaults.set<Config::ConfigValue>(entry.path, entry.defaultValue);
            }
        }
        LOG_INFO("App", __func__, "Config", "Applied config v3 catalog entries={} schemaVersion={} snapshotVersion={}",
                 payload.entries.size(), payload.schemaVersion, payload.snapshotVersion);
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("App", __func__, "Config", "Failed to apply config v3 catalog: {}", ex.what());
        return false;
    }
}

bool ServiceProxy::ApplyConfigV3SnapshotBytes(const uint8_t* data, size_t size) {
    try {
        const auto payload = Config::deserializeConfigV3Snapshot(data, size);
        std::unordered_map<Config::ConfigKeyId, const Config::ConfigSchemaEntry*> entryByKey;
        for (const auto& entry : m_configSchema.entries) {
            if (entry.keyId != Config::ConfigKeyId::MaxKeyId && !entry.yamlPath.empty()) {
                entryByKey.emplace(entry.keyId, &entry);
            }
        }
        size_t applied = 0;
        size_t dirtySkipped = 0;
        size_t unknownSkipped = 0;
        for (const auto& entry : payload.entries) {
            const auto it = entryByKey.find(entry.keyId);
            if (it == entryByKey.end()) {
                ++unknownSkipped;
                LOG_WARN("App", __func__, "Config", "Skipping config v3 snapshot entry with unknown keyId={}", static_cast<unsigned int>(entry.keyId));
                continue;
            }
            const auto& schemaEntry = *it->second;
            if (m_dirtyConfigPaths.contains(schemaEntry.yamlPath)) {
                ++dirtySkipped;
                continue;
            }
            const auto normalizedValue = NormalizeSnapshotValueForSchema(schemaEntry, entry.value);
            if (!normalizedValue.has_value()) {
                continue;
            }
            m_configStore.set<Config::ConfigValue>(schemaEntry.yamlPath, *normalizedValue);
            ++applied;
        }
        m_configV3SnapshotSchemaVersion = payload.schemaVersion;
        m_configV3SnapshotVersion = payload.snapshotVersion;
        SyncServiceMirrorsFromStore(
            m_configStore,
            m_srvDesiredModeFull,
            m_srvActiveModeFull,
            m_srvAutoMode,
            m_srvStylusVhfEnabled,
            m_srvPenButtonMode,
            m_srvPenButtonRoute);
        ApplyConfigStoreToLocalRuntime();
        LOG_INFO("App", __func__, "Config", "Applied config v3 snapshot entries={} applied={} dirtySkipped={} unknownSkipped={}",
                 payload.entries.size(), applied, dirtySkipped, unknownSkipped);
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("App", __func__, "Config", "Failed to apply config v3 snapshot: {}", ex.what());
        return false;
    }
}

bool ServiceProxy::ApplyConfigV3CatalogBytesForTest(const uint8_t* data, size_t size) {
    return ApplyConfigV3CatalogBytes(data, size);
}

bool ServiceProxy::ApplyConfigV3SnapshotBytesForTest(const uint8_t* data, size_t size) {
    return ApplyConfigV3SnapshotBytes(data, size);
}

ConfigV3BaselineVersions ServiceProxy::GetConfigV3BaselineVersionsForTest() const {
    return ConfigV3BaselineVersions{
        .catalogSchemaVersion = m_configV3CatalogSchemaVersion,
        .catalogSnapshotVersion = m_configV3CatalogSnapshotVersion,
        .snapshotSchemaVersion = m_configV3SnapshotSchemaVersion,
        .snapshotVersion = m_configV3SnapshotVersion,
    };
}

bool ServiceProxy::RefreshConfigCatalogV3() {
    if (!m_client.IsConnected()) return false;
    const auto bytes = FetchConfigV3Bytes(Ipc::ConfigV3PayloadKind::Catalog);
    return bytes.has_value() && ApplyConfigV3CatalogBytes(bytes->data(), bytes->size());
}

bool ServiceProxy::RefreshConfigSnapshotV3() {
    if (!IsConfigIpcConnectedForApply()) return false;
    const auto bytes = FetchConfigV3Bytes(Ipc::ConfigV3PayloadKind::Snapshot);
    return bytes.has_value() && ApplyConfigV3SnapshotBytes(bytes->data(), bytes->size());
}

void ServiceProxy::RefreshConfigSnapshot() {
    if (!m_client.IsConnected()) return;
    if (!RefreshConfigSnapshotV3()) {
        LOG_WARN("App", __func__, "Config", "Config v3 snapshot unavailable; keeping current app-local config store.");
    }
}

void ServiceProxy::SetSrvModeFull(bool full) {
    if (!IsLiveControlAllowed()) return;
    m_srvDesiredModeFull.store(full, std::memory_order_relaxed);
    SetValue(m_configStore, "service.mode", ToConfigValue(ToServiceModeConfig(full)));
    m_dirtyConfigPaths.insert("service.mode");
}

void ServiceProxy::SetSrvStylusVhfEnabled(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvStylusVhfEnabled.store(enabled, std::memory_order_relaxed);
    SetValue(m_configStore, "service.stylus_vhf_enabled", ToConfigValue(enabled));
    m_dirtyConfigPaths.insert("service.stylus_vhf_enabled");
}

void ServiceProxy::SetSrvAutoMode(bool enabled) {
    if (!IsLiveControlAllowed()) return;
    m_srvAutoMode.store(enabled, std::memory_order_relaxed);
    SetValue(m_configStore, "service.auto_mode", ToConfigValue(enabled));
    m_dirtyConfigPaths.insert("service.auto_mode");
}

void ServiceProxy::SetPenButtonMode(PenButtonMode m) {
    if (!IsLiveControlAllowed()) return;
    m_srvPenButtonMode.store(m, std::memory_order_relaxed);
    SetValue(m_configStore, "service.pen_button_mode", ToConfigValue(m));
    m_dirtyConfigPaths.insert("service.pen_button_mode");
}

void ServiceProxy::SetPenButtonRoute(PenButtonRoute r) {
    if (!IsLiveControlAllowed()) return;
    m_srvPenButtonRoute.store(r, std::memory_order_relaxed);
    SetValue(m_configStore, "service.pen_button_route", ToConfigValue(r));
    m_dirtyConfigPaths.insert("service.pen_button_route");
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
