#include "ServiceProxyInternal.h"

#include <cstring>

namespace App {

TouchPipelineModuleEnableState CaptureTouchPipelineModuleEnableState(
    const Solvers::TouchPipeline& pipeline) {
    TouchPipelineModuleEnableState state;
    state.baselineEnabled = pipeline.m_baseline.m_enabled;
    state.cmfEnabled = pipeline.m_cmf.m_enabled;
    state.trackerEnabled = pipeline.m_tracker.m_enabled;
    state.coordFilterEnabled = pipeline.m_coordFilter.m_enabled;
    state.gestureEnabled = pipeline.m_gesture.m_enabled;
    return state;
}

void ApplyTouchPipelineModuleEnableState(
    Solvers::TouchPipeline& pipeline,
    const TouchPipelineModuleEnableState& state) {
    pipeline.m_baseline.m_enabled = state.baselineEnabled;
    pipeline.m_cmf.m_enabled = state.cmfEnabled;
    pipeline.m_tracker.m_enabled = state.trackerEnabled;
    pipeline.m_coordFilter.m_enabled = state.coordFilterEnabled;
    pipeline.m_gesture.m_enabled = state.gestureEnabled;
}

namespace {

using RuntimeConfigValueType = Dvr::Format::Dvr2ConfigValueType;

RuntimeConfigValueType ToRuntimeConfigValueType(Solvers::ConfigParam::Type type) {
    switch (type) {
    case Solvers::ConfigParam::Bool: return RuntimeConfigValueType::Bool;
    case Solvers::ConfigParam::Int: return RuntimeConfigValueType::Int32;
    case Solvers::ConfigParam::UInt8: return RuntimeConfigValueType::UInt8;
    case Solvers::ConfigParam::UInt16: return RuntimeConfigValueType::UInt16;
    case Solvers::ConfigParam::UInt32: return RuntimeConfigValueType::UInt32;
    case Solvers::ConfigParam::Float: return RuntimeConfigValueType::Float32;
    case Solvers::ConfigParam::Double: return RuntimeConfigValueType::Float64;
    case Solvers::ConfigParam::String: return RuntimeConfigValueType::String;
    }
    return RuntimeConfigValueType::String;
}

uint64_t PackFloat32(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint64_t PackFloat64(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void AppendRuntimeConfigValue(DvrRuntimeConfigSnapshot& snapshot,
                              std::string section,
                              std::string key,
                              std::string displayName,
                              RuntimeConfigValueType valueType,
                              uint64_t rawValue,
                              std::string stringValue = {},
                              uint8_t category = static_cast<uint8_t>(Solvers::ConfigParam::General),
                              float minValue = 0.0f,
                              float maxValue = 0.0f,
                              std::string moduleTag = {},
                              std::string unit = {}) {
    const uint32_t fieldId = static_cast<uint32_t>(snapshot.fields.size() + 1);
    DvrRuntimeConfigField field{};
    field.fieldId = fieldId;
    field.valueType = valueType;
    field.category = category;
    field.minValue = minValue;
    field.maxValue = maxValue;
    field.section = std::move(section);
    field.key = std::move(key);
    field.displayName = std::move(displayName);
    field.moduleTag = std::move(moduleTag);
    field.unit = std::move(unit);
    snapshot.fields.push_back(std::move(field));

    DvrRuntimeConfigValue value{};
    value.fieldId = fieldId;
    value.valueType = valueType;
    value.valid = true;
    value.rawValue = rawValue;
    value.stringValue = std::move(stringValue);
    snapshot.values.push_back(std::move(value));
}

void AppendPipelineRuntimeConfig(DvrRuntimeConfigSnapshot& snapshot,
                                 std::string_view section,
                                 const std::vector<Solvers::ConfigParam>& schema) {
    for (const auto& param : schema) {
        const auto valueType = ToRuntimeConfigValueType(param.type);
        uint64_t rawValue = 0;
        std::string stringValue;
        if (param.valuePtr) {
            switch (param.type) {
            case Solvers::ConfigParam::Bool:
                rawValue = *static_cast<const bool*>(param.valuePtr) ? 1ull : 0ull;
                break;
            case Solvers::ConfigParam::Int:
                rawValue = static_cast<uint32_t>(*static_cast<const int*>(param.valuePtr));
                break;
            case Solvers::ConfigParam::UInt8:
                rawValue = *static_cast<const uint8_t*>(param.valuePtr);
                break;
            case Solvers::ConfigParam::UInt16:
                rawValue = *static_cast<const uint16_t*>(param.valuePtr);
                break;
            case Solvers::ConfigParam::UInt32:
                rawValue = *static_cast<const uint32_t*>(param.valuePtr);
                break;
            case Solvers::ConfigParam::Float:
                rawValue = PackFloat32(*static_cast<const float*>(param.valuePtr));
                break;
            case Solvers::ConfigParam::Double:
                rawValue = PackFloat64(*static_cast<const double*>(param.valuePtr));
                break;
            case Solvers::ConfigParam::String:
                stringValue = *static_cast<const std::string*>(param.valuePtr);
                rawValue = static_cast<uint64_t>(stringValue.size());
                break;
            }
        }

        AppendRuntimeConfigValue(snapshot,
                                 std::string(section),
                                 param.key,
                                 param.displayName,
                                 valueType,
                                 rawValue,
                                 std::move(stringValue),
                                 static_cast<uint8_t>(param.category),
                                 param.minVal,
                                 param.maxVal,
                                 param.moduleTag);
    }
}

std::string ModeString(bool full) {
    return full ? "full" : "touch_only";
}

} // namespace

DvrRuntimeConfigSnapshot BuildRuntimeConfigSnapshotFromState(
    const ServiceRuntimeConfigState& serviceState,
    const AppRuntimeConfigState& appRuntimeState,
    const Solvers::TouchPipeline& touchPipeline,
    const Solvers::StylusPipeline& stylusPipeline) {
    DvrRuntimeConfigSnapshot snapshot;

    AppendRuntimeConfigValue(snapshot, "Service", "desired_mode", "Desired Service Mode", RuntimeConfigValueType::String, 0, ModeString(serviceState.desiredModeFull));
    AppendRuntimeConfigValue(snapshot, "Service", "active_mode", "Active Service Mode", RuntimeConfigValueType::String, 0, ModeString(serviceState.activeModeFull));
    AppendRuntimeConfigValue(snapshot, "Service", "auto_mode", "Service Auto Mode", RuntimeConfigValueType::Bool, serviceState.autoMode ? 1ull : 0ull);
    AppendRuntimeConfigValue(snapshot, "Service", "stylus_vhf_enabled", "Stylus VHF Enabled", RuntimeConfigValueType::Bool, serviceState.stylusVhfEnabled ? 1ull : 0ull);
    AppendRuntimeConfigValue(snapshot, "Service", "pen_button_mode", "Pen Button Mode", RuntimeConfigValueType::UInt8, static_cast<uint8_t>(serviceState.penButtonMode));
    AppendRuntimeConfigValue(snapshot, "Service", "pen_button_route", "Pen Button Route", RuntimeConfigValueType::UInt8, static_cast<uint8_t>(serviceState.penButtonRoute));
    AppendRuntimeConfigValue(snapshot, "AppRuntime", "vhf_enabled", "Touch VHF Enabled", RuntimeConfigValueType::Bool, appRuntimeState.vhfEnabled ? 1ull : 0ull);
    AppendRuntimeConfigValue(snapshot, "AppRuntime", "vhf_transpose", "Touch VHF Transpose", RuntimeConfigValueType::Bool, appRuntimeState.vhfTranspose ? 1ull : 0ull);
    AppendRuntimeConfigValue(snapshot, "AppRuntime", "master_parser_only", "Master Parser Only", RuntimeConfigValueType::Bool, appRuntimeState.masterParserOnly ? 1ull : 0ull);

    AppendPipelineRuntimeConfig(snapshot, "TouchPipeline", touchPipeline.GetConfigSchema());
    AppendPipelineRuntimeConfig(snapshot, "StylusPipeline", stylusPipeline.GetConfigSchema());
    return snapshot;
}

DvrRuntimeConfigSnapshot ServiceProxy::CaptureRuntimeConfigSnapshot() const {
    ServiceRuntimeConfigState serviceState{};
    serviceState.desiredModeFull = m_srvDesiredModeFull.load(std::memory_order_relaxed);
    serviceState.activeModeFull = m_srvActiveModeFull.load(std::memory_order_relaxed);
    serviceState.autoMode = m_srvAutoMode.load(std::memory_order_relaxed);
    serviceState.stylusVhfEnabled = m_srvStylusVhfEnabled.load(std::memory_order_relaxed);
    serviceState.penButtonMode = m_srvPenButtonMode.load(std::memory_order_relaxed);
    serviceState.penButtonRoute = m_srvPenButtonRoute.load(std::memory_order_relaxed);

    AppRuntimeConfigState appRuntimeState{};
    appRuntimeState.vhfEnabled = m_vhfEnabled.load(std::memory_order_relaxed);
    appRuntimeState.vhfTranspose = m_vhfTranspose.load(std::memory_order_relaxed);
    appRuntimeState.masterParserOnly = m_masterParserOnly.load(std::memory_order_relaxed);

    return BuildRuntimeConfigSnapshotFromState(serviceState, appRuntimeState, m_pipeline, m_stylusPipeline);
}


} // namespace App
