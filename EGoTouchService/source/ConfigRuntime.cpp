#include "ConfigRuntime.h"

#include "Logger.h"
#include "SolverTypes.h"
#include "TouchPipeline.h"
#include "StylusPipeline.h"
#include "config/ConfigBinder.h"
#include "config/ConfigCatalog.h"
#include "config/ConfigKeyMap.h"
#include "config/SchemaValidator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Service {
namespace {

const char* ToConfigValue(PenButtonMode mode) {
    switch (mode) {
    case PenButtonMode::OemCustom: return "oem_custom";
    case PenButtonMode::NativeBarrel: return "native_barrel";
    case PenButtonMode::NativeEraser: return "native_eraser";
    }
    return "oem_custom";
}

const char* ToConfigValue(PenButtonRoute route) {
    switch (route) {
    case PenButtonRoute::VhfOnly: return "vhf_only";
    case PenButtonRoute::Win32Only: return "win32_only";
    case PenButtonRoute::VhfAndWin32: return "vhf_and_win32";
    }
    return "vhf_only";
}

Config::ConfigValue ConfigValueFromTlvEntry(const Config::ConfigTlvEntry& entry, bool& ok) {
    ok = true;
    try {
        switch (entry.valueType) {
        case Config::ConfigValueType::Bool:
            if (entry.stringValue == "true" || entry.stringValue == "1") return Config::ConfigValue(true);
            if (entry.stringValue == "false" || entry.stringValue == "0") return Config::ConfigValue(false);
            ok = false; return Config::ConfigValue(false);
        case Config::ConfigValueType::Int32: {
            size_t pos = 0;
            const int parsed = std::stoi(entry.stringValue, &pos);
            if (pos != entry.stringValue.size()) { ok = false; return Config::ConfigValue(int32_t{0}); }
            return Config::ConfigValue(static_cast<int32_t>(parsed));
        }
        case Config::ConfigValueType::Float: {
            size_t pos = 0;
            const float parsed = std::stof(entry.stringValue, &pos);
            if (pos != entry.stringValue.size() || !std::isfinite(parsed)) { ok = false; return Config::ConfigValue(0.0f); }
            return Config::ConfigValue(parsed);
        }
        case Config::ConfigValueType::String:
            return Config::ConfigValue(entry.stringValue);
        case Config::ConfigValueType::Null:
        default:
            ok = false; return Config::ConfigValue(std::string{});
        }
    } catch (const std::exception&) {
        ok = false; return Config::ConfigValue(std::string{});
    }
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kStylusIirCoefficientPathPairs{
    std::pair{"stylus.sp.iir_coef_low_hover", "stylus.sp.iir_coef_low_in_band"},
    std::pair{"stylus.sp.iir_coef_high_hover", "stylus.sp.iir_coef_high_in_band"},
    std::pair{"stylus.sp.iir_coef_low_writing", "stylus.sp.iir_coef_low_edge"},
    std::pair{"stylus.sp.iir_coef_high_writing", "stylus.sp.iir_coef_high_edge"},
};

bool StylusIirCoefficientsWithinMax(const Config::ConfigStore& store) {
    const int32_t maxCoef = store.getOr<int32_t>("stylus.sp.iir_max_coef", 32);
    if (maxCoef < 1) return false;
    for (const auto [canonicalPath, legacyPath] : kStylusIirCoefficientPathPairs) {
        const auto path = store.has(canonicalPath) ? canonicalPath : legacyPath;
        const int32_t coef = store.getOr<int32_t>(path, 0);
        if (coef < 0 || coef > maxCoef) return false;
    }
    return true;
}

void ClampStylusIirCoefficients(Config::ConfigStore& store) {
    const int32_t maxCoef = std::clamp(store.getOr<int32_t>("stylus.sp.iir_max_coef", 32), int32_t{1}, int32_t{255});
    store.set<int32_t>("stylus.sp.iir_max_coef", maxCoef);
    for (const auto [canonicalPath, legacyPath] : kStylusIirCoefficientPathPairs) {
        const auto path = store.has(canonicalPath) ? canonicalPath : legacyPath;
        if (store.has(path)) store.set<int32_t>(path, std::clamp(store.get<int32_t>(path), int32_t{0}, maxCoef));
    }
}

bool ConfigValueAllowedBySchema(std::string_view path, const Config::ConfigValue& value, const Config::ConfigSchemaSnapshot& schema) {
    const auto it = std::find_if(schema.entries.begin(), schema.entries.end(),
        [path](const Config::ConfigSchemaEntry& entry) { return entry.yamlPath == path; });
    if (it == schema.entries.end()) return false;
    if (!it->boundToRuntime ||
        (it->runtimeBinding != Config::ConfigRuntimeBinding::LiveSetter &&
         it->runtimeBinding != Config::ConfigRuntimeBinding::ManualLiveApply)) return false;
    if (path == "service.mode") {
        const auto str = Config::tryGetValue<std::string>(value);
        return str.has_value() && (*str == "full" || *str == "touch_only");
    }
    if (path == "service.pen_button_mode") return ParsePenButtonModeValue(value).has_value();
    if (path == "service.pen_button_route") return ParsePenButtonRouteValue(value).has_value();
    switch (it->uiType) {
    case Config::ConfigUiType::Bool: return Config::tryGetValue<bool>(value).has_value();
    case Config::ConfigUiType::Int32: {
        const auto v = Config::tryGetValue<int32_t>(value);
        return v.has_value() && (!it->range.has_value() || (*v >= it->range->min && *v <= it->range->max));
    }
    case Config::ConfigUiType::Float: {
        const auto v = Config::tryGetValue<float>(value);
        return v.has_value() && (!it->range.has_value() || (*v >= it->range->min && *v <= it->range->max));
    }
    case Config::ConfigUiType::Enum:
    case Config::ConfigUiType::String: return Config::tryGetValue<std::string>(value).has_value();
    }
    return false;
}

} // namespace

bool ConfigRuntime::Initialize(const std::string& configPath, const StartupValidator& validateStartupConfig) {
    Config::ConfigBinder binder;
    ServiceConfigState serviceDefaults;
    Solvers::TouchPipeline touchDefaults;
    Solvers::StylusPipeline stylusDefaults;
    RegisterServiceConfigBindings(binder, serviceDefaults);
    touchDefaults.registerBindings(binder);
    stylusDefaults.registerBindings(binder);
    Config::registerRuntimeKeyMappings(binder);

    Config::ConfigStore defaults;
    defaults.set<std::string>("service.mode", "full");
    defaults.set<bool>("service.auto_mode", true);
    defaults.set<bool>("service.stylus_vhf_enabled", true);
    defaults.set<std::string>("service.pen_button_mode", "oem_custom");
    defaults.set<std::string>("service.pen_button_route", "vhf_only");
    binder.writeDefaults(defaults);

    std::optional<std::string> cliConfigPath;
    if (!configPath.empty()) cliConfigPath = configPath;
    auto paths = Config::resolve(cliConfigPath);
#if EGOTOUCH_CONFIG_ENABLED
    if (!paths.has_value()) {
        LOG_ERROR("Service", __func__, "Config", "config/default.yaml was not resolved; startup blocked.");
        return false;
    }
#endif
    if (paths.has_value()) {
        try {
            Config::ConfigStore yamlDefaults;
            yamlDefaults.loadFromYaml(paths->defaultConfig);
            defaults.mergeFrom(yamlDefaults);
            ApplyLegacyServiceModeMigration(defaults, yamlDefaults);
        } catch (const std::exception& ex) {
            LOG_WARN("Service", __func__, "Config", "Failed to load default config '{}': {}", paths->defaultConfig, ex.what());
        }
    }

    Config::ConfigStore current;
    current.mergeFrom(defaults);
    bool penButtonRouteExplicit = false;
    if (paths.has_value() && paths->overrideExists) {
        try {
            Config::ConfigStore overrides;
            overrides.loadFromYaml(paths->overrideConfig);
            penButtonRouteExplicit = overrides.has("service.pen_button_route");
            current.mergeFrom(overrides);
            ApplyLegacyServiceModeMigration(current, overrides);
        } catch (const std::exception& ex) {
            LOG_WARN("Service", __func__, "Config", "Failed to load override config '{}': {}", paths->overrideConfig, ex.what());
        }
    }

    if (!ValidateStartupConfig(current, validateStartupConfig)) {
        LOG_ERROR("Service", __func__, "Config", "Startup config schema validation failed; startup blocked.");
        return false;
    }

    auto schema = Config::BuildMergedSchema(defaults, binder);
    for (const auto& path : current.allPaths()) {
        const auto value = current.get<Config::ConfigValue>(path);
        if (!ConfigValueAllowedBySchema(path, value, schema) && defaults.has(path)) {
            current.set<Config::ConfigValue>(path, defaults.get<Config::ConfigValue>(path));
            LOG_WARN("Service", __func__, "Config", "Invalid config value at '{}'; restored default.", path);
        }
    }
    if (!StylusIirCoefficientsWithinMax(current)) {
        ClampStylusIirCoefficients(current);
        LOG_WARN("Service", __func__, "Config", "Invalid stylus IIR coefficient/max relationship; clamped coefficients to max.");
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    m_defaults = std::move(defaults);
    m_store = std::move(current);
    m_schema = std::move(schema);
    m_paths = std::move(paths);
    m_penButtonRouteExplicit = penButtonRouteExplicit;
    auto state = ReadServiceConfigStateFromStoreLocked();
    WriteServiceConfigStateToStoreLocked(state);
    return true;
}

uint32_t HashBytes(std::span<const uint8_t> bytes) noexcept {
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

ConfigRuntime::ConfigV3Blob ConfigRuntime::BuildCatalogV3Blob() const {
    Config::ConfigSchemaSnapshot schema;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        schema = m_schema;
    }

    Config::ConfigV3CatalogPayload payload{};
    payload.entries = Config::ConfigCatalogBuilder::fromSnapshot(schema).descriptors();
    payload.entries.erase(std::remove_if(payload.entries.begin(), payload.entries.end(),
        [](const Config::ConfigDescriptor& entry) { return entry.keyId == Config::ConfigKeyId::MaxKeyId; }),
        payload.entries.end());

    const auto schemaBytes = Config::serializeConfigV3Catalog(payload);
    payload.schemaVersion = HashBytes(schemaBytes);
    payload.snapshotVersion = 0;
    auto bytes = Config::serializeConfigV3Catalog(payload);
    const uint32_t checksum = HashBytes(bytes);
    return ConfigV3Blob{std::move(bytes), payload.schemaVersion, payload.snapshotVersion, checksum};
}

ConfigRuntime::ConfigV3Blob ConfigRuntime::BuildSnapshotV3Blob() const {
    Config::ConfigStore store;
    Config::ConfigSchemaSnapshot schema;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        store = m_store;
        schema = m_schema;
    }

    Config::ConfigV3SnapshotPayload payload{};
    auto descriptors = Config::ConfigCatalogBuilder::fromSnapshot(schema).descriptors();
    for (const auto& descriptor : descriptors) {
        if (descriptor.keyId == Config::ConfigKeyId::MaxKeyId) continue;
        Config::ConfigV3SnapshotEntry entry{};
        entry.keyId = descriptor.keyId;
        entry.value = store.getOr<Config::ConfigValue>(descriptor.path, descriptor.defaultValue);
        payload.entries.push_back(std::move(entry));
    }

    Config::ConfigV3CatalogPayload catalogPayload{};
    catalogPayload.entries = std::move(descriptors);
    catalogPayload.entries.erase(std::remove_if(catalogPayload.entries.begin(), catalogPayload.entries.end(),
        [](const Config::ConfigDescriptor& entry) { return entry.keyId == Config::ConfigKeyId::MaxKeyId; }),
        catalogPayload.entries.end());
    payload.schemaVersion = HashBytes(Config::serializeConfigV3Catalog(catalogPayload));
    payload.snapshotVersion = HashBytes(Config::serializeConfigV3Snapshot(payload));
    auto bytes = Config::serializeConfigV3Snapshot(payload);
    const uint32_t checksum = HashBytes(bytes);
    return ConfigV3Blob{std::move(bytes), payload.schemaVersion, payload.snapshotVersion, checksum};
}

bool ConfigRuntime::ValidateStartupConfig(const Config::ConfigStore& store, const StartupValidator& validateStartupConfig) const {
    // Phase 1 runs before DeviceRuntime exists: validate service-owned config keys here,
    // then delegate pipeline validation to the startup callback when one is supplied.
    ServiceConfigState schemaState{};
    Config::ConfigBinder serviceBinder;
    RegisterServiceConfigBindings(serviceBinder, schemaState);
    auto serviceValidation = Config::SchemaValidator::validate(store, serviceBinder);
    serviceValidation.logAll();
    if (!serviceValidation.ok()) return false;
    return !validateStartupConfig || validateStartupConfig(store);
}

Config::ConfigStore ConfigRuntime::SnapshotStore() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_store;
}

ServiceConfigState ConfigRuntime::ServiceState() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return ReadServiceConfigStateFromStoreLocked();
}

void ConfigRuntime::WriteServiceState(const ServiceConfigState& config) {
    std::lock_guard<std::mutex> lk(m_mutex);
    WriteServiceConfigStateToStoreLocked(config);
}

ServiceConfigState ConfigRuntime::ReadServiceConfigStateFromStoreLocked() const {
    ServiceConfigState state{};
    state.penButtonRouteExplicit = m_penButtonRouteExplicit;
    ApplyConfig(state, m_store);
    state.penButtonRouteExplicit = m_penButtonRouteExplicit;
    return state;
}

void ConfigRuntime::WriteServiceConfigStateToStoreLocked(const ServiceConfigState& config) {
    m_store.set<std::string>("service.mode", ServiceModeToConfig(config.mode));
    m_store.set<bool>("service.auto_mode", config.autoMode);
    m_store.set<bool>("service.stylus_vhf_enabled", config.stylusVhfEnabled);
    m_store.set<std::string>("service.pen_button_mode", ToConfigValue(config.penButtonMode));
    m_store.set<std::string>("service.pen_button_route", ToConfigValue(config.penButtonRoute));
    m_penButtonRouteExplicit = config.penButtonRouteExplicit;
}

bool ConfigRuntime::PersistServicePolicyConfig(const ServiceConfigState& config) {
#if EGOTOUCH_CONFIG_ENABLED
    Config::ConfigStore storeToPersist;
    Config::ConfigStore defaultsToPersist;
    Config::ConfigPaths pathsToPersist;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_paths.has_value()) {
            LOG_ERROR("Service", __func__, "Config", "Cannot persist config before startup paths are resolved.");
            return false;
        }
        WriteServiceConfigStateToStoreLocked(config);
        storeToPersist = m_store;
        defaultsToPersist = m_defaults;
        pathsToPersist = *m_paths;
    }
    try {
        storeToPersist.saveOverrides(pathsToPersist.overrideConfig, defaultsToPersist);
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("Service", __func__, "Config", "PersistConfig failed: {}", ex.what());
        return false;
    }
#else
    (void)config;
    return false;
#endif
}

ConfigRuntime::TlvApplyResult ConfigRuntime::ApplyTlvChunk(const Ipc::ConfigTlvChunkRequestWire& chunk) {
    TlvApplyResult result{};
    if (chunk.wireVersion != Ipc::kIpcProtocolVersion ||
        chunk.totalLen == 0 || chunk.totalLen > Ipc::kConfigTlvMaxPayloadBytes ||
        chunk.chunkLen > Ipc::kConfigTlvChunkPayloadBytes ||
        static_cast<uint32_t>(chunk.offset) + chunk.chunkLen > chunk.totalLen) {
        result.status = Ipc::IpcStatusCode::InvalidRequest;
        return result;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    const bool first = (chunk.flags & Ipc::kConfigTlvChunkFirst) != 0;
    if (first) {
        m_pendingTlvSessionId = chunk.sessionId;
        m_pendingTlvTotalLen = chunk.totalLen;
        m_pendingTlvReceived = 0;
        m_pendingTlvPayload.assign(chunk.totalLen, uint8_t{0});
    }
    if (m_pendingTlvSessionId != chunk.sessionId ||
        m_pendingTlvTotalLen != chunk.totalLen ||
        m_pendingTlvPayload.size() != chunk.totalLen ||
        chunk.offset != m_pendingTlvReceived) {
        result.status = Ipc::IpcStatusCode::InvalidRequest;
        return result;
    }
    std::memcpy(m_pendingTlvPayload.data() + chunk.offset, chunk.bytes, chunk.chunkLen);
    m_pendingTlvReceived = static_cast<uint16_t>(m_pendingTlvReceived + chunk.chunkLen);
    if ((chunk.flags & Ipc::kConfigTlvChunkLast) == 0) return result;
    if (m_pendingTlvReceived != m_pendingTlvTotalLen) {
        result.status = Ipc::IpcStatusCode::InvalidRequest;
        return result;
    }
    const auto payload = m_pendingTlvPayload;
    m_pendingTlvPayload.clear();
    m_pendingTlvSessionId = 0;
    m_pendingTlvTotalLen = 0;
    m_pendingTlvReceived = 0;
    result.completed = true;
    ApplyCompletedTlvPayloadLocked(payload, result);
    return result;
}

bool ConfigRuntime::ApplyCompletedTlvPayloadLocked(const std::vector<uint8_t>& payload, TlvApplyResult& result) {
    const auto parseResult = Config::deserializePatchDetailed(payload.data(), payload.size());
    if (!parseResult.ok()) {
        result.status = parseResult.status == Config::ConfigTlvParseStatus::InvalidArgument
            ? Ipc::IpcStatusCode::InternalError
            : Ipc::IpcStatusCode::InvalidRequest;
        LOG_WARN("Service", __func__, "Config", "Rejected config TLV patch: status={} offset={} entryIndex={} keyId=0x{:04X} valueType=0x{:02X}",
                 Config::toString(parseResult.status), parseResult.issue.offset, parseResult.issue.entryIndex,
                 parseResult.issue.rawKeyId, parseResult.issue.rawValueType);
        return false;
    }
    const auto& patch = parseResult.patch;
    bool penButtonRouteTouched = false;
    Config::ConfigStore candidate = m_store;
    for (const auto& entry : patch.entries) {
        const auto path = Config::tryPathForKeyId(entry.keyId);
        if (!path.has_value()) { result.status = Ipc::IpcStatusCode::InvalidRequest; return false; }
        bool valueOk = false;
        const Config::ConfigValue value = ConfigValueFromTlvEntry(entry, valueOk);
        if (!valueOk) { result.status = Ipc::IpcStatusCode::InvalidRequest; return false; }
        const std::string pathString(*path);
        if (pathString == "service.pen_button_route") penButtonRouteTouched = true;
        if (!ConfigValueAllowedBySchema(pathString, value, m_schema)) { result.status = Ipc::IpcStatusCode::InvalidRequest; return false; }
        const bool changed = !candidate.has(pathString) || candidate.get<Config::ConfigValue>(pathString) != value;
        candidate.set<Config::ConfigValue>(pathString, value);
        if (changed) ++result.changedCount;
    }
    if (!StylusIirCoefficientsWithinMax(candidate)) { result.status = Ipc::IpcStatusCode::InvalidRequest; return false; }

    result.desiredServiceConfig = ReadServiceConfigStateFromStoreLocked();
    result.desiredServiceConfig.penButtonRouteExplicit = m_penButtonRouteExplicit;
    ApplyConfig(result.desiredServiceConfig, candidate);
    result.desiredServiceConfig.penButtonRouteExplicit = m_penButtonRouteExplicit || penButtonRouteTouched;
    m_store = candidate;
    m_penButtonRouteExplicit = result.desiredServiceConfig.penButtonRouteExplicit;
    result.pipelineConfig = m_store;
    result.entryCount = patch.entries.size();
    result.status = Ipc::IpcStatusCode::Ok;
    return true;
}

} // namespace Service
