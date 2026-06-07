#include "ConfigRuntime.h"

#include "Logger.h"
#include "SolverTypes.h"
#include "config/ConfigBinder.h"
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

namespace Service {
namespace {

void RegisterServiceConfigBindings(Config::ConfigBinder& binder, ServiceConfigState& state) {
    static const std::array<std::pair<ServiceMode, std::string>, 2> kModeMapping{{
        {ServiceMode::Full, "full"},
        {ServiceMode::TouchOnly, "touch_only"},
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
    binder.bindEnum("service.mode", &ServiceConfigState::mode, state,
                    ServiceMode::Full, std::span<const std::pair<ServiceMode, std::string>>(kModeMapping), "Service runtime topology", runtimeBinding);
    binder.bind("service.auto_mode", &ServiceConfigState::autoMode, state,
                true, {}, "Enable automatic runtime start/init", runtimeBinding);
    binder.bind("service.stylus_vhf_enabled", &ServiceConfigState::stylusVhfEnabled, state,
                true, {}, "Enable stylus VHF output", runtimeBinding);
    binder.bindEnum("service.pen_button_mode", &ServiceConfigState::penButtonMode, state,
                    PenButtonMode::OemCustom, std::span<const std::pair<PenButtonMode, std::string>>(kPenButtonModeMapping), "Pen button semantic mode", runtimeBinding);
    binder.bindEnum("service.pen_button_route", &ServiceConfigState::penButtonRoute, state,
                    PenButtonRoute::VhfOnly, std::span<const std::pair<PenButtonRoute, std::string>>(kPenButtonRouteMapping), "Pen button injection route", runtimeBinding);
}

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

constexpr std::array<std::string_view, 4> kStylusIirCoefficientPaths{
    "stylus.sp.iir_coef_low_in_band",
    "stylus.sp.iir_coef_high_in_band",
    "stylus.sp.iir_coef_low_edge",
    "stylus.sp.iir_coef_high_edge",
};

bool StylusIirCoefficientsWithinMax(const Config::ConfigStore& store) {
    const int32_t maxCoef = store.getOr<int32_t>("stylus.sp.iir_max_coef", 32);
    if (maxCoef < 1) return false;
    for (const auto path : kStylusIirCoefficientPaths) {
        const int32_t coef = store.getOr<int32_t>(path, 0);
        if (coef < 0 || coef > maxCoef) return false;
    }
    return true;
}

void ClampStylusIirCoefficients(Config::ConfigStore& store) {
    const int32_t maxCoef = std::clamp(store.getOr<int32_t>("stylus.sp.iir_max_coef", 32), int32_t{1}, int32_t{255});
    store.set<int32_t>("stylus.sp.iir_max_coef", maxCoef);
    for (const auto path : kStylusIirCoefficientPaths) {
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

bool ConfigRuntime::ValidateStartupConfig(const Config::ConfigStore& store, const StartupValidator& validateStartupConfig) const {
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
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_paths.has_value()) {
        LOG_ERROR("Service", __func__, "Config", "Cannot persist config before startup paths are resolved.");
        return false;
    }
    try {
        WriteServiceConfigStateToStoreLocked(config);
        m_store.saveOverrides(m_paths->overrideConfig, m_defaults);
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
    const auto patch = Config::deserializePatch(payload.data(), payload.size());
    if (patch.entries.empty()) { result.status = Ipc::IpcStatusCode::InvalidRequest; return false; }
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
