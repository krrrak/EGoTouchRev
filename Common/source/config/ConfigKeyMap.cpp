#include "config/ConfigKeyMap.h"

#include "config/ConfigBinder.h"
#include "config/ConfigStore.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Config {
namespace {

const std::pair<ConfigKeyId, std::string_view> kStaticKeyMap[] = {
    {ConfigKeyId::SvcMode, "service.mode"},
    {ConfigKeyId::SvcAutoMode, "service.auto_mode"},
    {ConfigKeyId::SvcStylusVhfEnabled, "service.stylus_vhf_enabled"},
    {ConfigKeyId::SvcPenButtonMode, "service.pen_button_mode"},
    {ConfigKeyId::SvcPenButtonRoute, "service.pen_button_route"},

    {ConfigKeyId::TouchBaselineBgAlphaShift, "touch.signal_cond.baseline_bg_alpha_shift"},
    {ConfigKeyId::TouchBaselineBgMaxStep, "touch.signal_cond.baseline_bg_max_step"},
    {ConfigKeyId::TouchBaselineNoFingerAlphaShift, "touch.signal_cond.baseline_no_finger_alpha_shift"},
    {ConfigKeyId::TouchBaselineNoFingerMaxStep, "touch.signal_cond.baseline_no_finger_max_step"},
    {ConfigKeyId::TouchBaselineRecoveryAlphaShift, "touch.signal_cond.baseline_recovery_alpha_shift"},
    {ConfigKeyId::TouchBaselineRecoveryMaxFrames, "touch.signal_cond.baseline_recovery_max_frames"},
    {ConfigKeyId::TouchBaselineRecoveryMaxStep, "touch.signal_cond.baseline_recovery_max_step"},
    {ConfigKeyId::TouchFrameParserEnabled, "touch.frame_parser.enabled"},

    {ConfigKeyId::StylusHpp2Enabled, "stylus.hpp2.enabled"},
    {ConfigKeyId::StylusHpp2SensorTxCount, "stylus.hpp2.sensor_tx_count"},
    {ConfigKeyId::StylusHpp2SensorRxCount, "stylus.hpp2.sensor_rx_count"},
    {ConfigKeyId::StylusHpp2CmfWindowRadius, "stylus.hpp2.cmf_window_radius"},
    {ConfigKeyId::StylusHpp2RawAbnormalLineSumThreshold, "stylus.hpp2.raw_abnormal_line_sum_threshold"},
    {ConfigKeyId::StylusHpp2RawAbnormalEnergyRatioThreshold, "stylus.hpp2.raw_abnormal_energy_ratio_threshold"},
    {ConfigKeyId::StylusHpp2CmnAbnormalSumThreshold, "stylus.hpp2.cmn_abnormal_sum_threshold"},
    {ConfigKeyId::StylusHpp2CmnAbnormalMinThreshold, "stylus.hpp2.cmn_abnormal_min_threshold"},
    {ConfigKeyId::StylusHpp2ChargerNoiseClearFloor, "stylus.hpp2.charger_noise_clear_floor"},
    {ConfigKeyId::StylusHpp2ChargerNoiseRatioThreshold, "stylus.hpp2.charger_noise_ratio_threshold"},
    {ConfigKeyId::StylusHpp2ChargerNoiseSumThreshold, "stylus.hpp2.charger_noise_sum_threshold"},
    {ConfigKeyId::StylusHpp2ChargerNoiseMaxSampleThreshold, "stylus.hpp2.charger_noise_max_sample_threshold"},
    {ConfigKeyId::StylusHpp2ChargerNoiseAbnormalChannelThreshold, "stylus.hpp2.charger_noise_abnormal_channel_threshold"},
    {ConfigKeyId::StylusHpp2ChargerNoisePeakProtectRadius, "stylus.hpp2.charger_noise_peak_protect_radius"},
    {ConfigKeyId::StylusHpp2ChargerNoiseMinRawSample, "stylus.hpp2.charger_noise_min_raw_sample"},
    {ConfigKeyId::StylusHpp2PeakSignalFloor, "stylus.hpp2.peak_signal_floor"},
    {ConfigKeyId::StylusHpp2PeakSearchNeighborDist, "stylus.hpp2.peak_search_neighbor_dist"},
    {ConfigKeyId::StylusHpp2PeakMinWidth, "stylus.hpp2.peak_min_width"},
    {ConfigKeyId::StylusHpp2PeakMaxWidth, "stylus.hpp2.peak_max_width"},
    {ConfigKeyId::StylusHpp2PressureEdgeEnterThreshold, "stylus.hpp2.pressure_edge_enter_threshold"},
    {ConfigKeyId::StylusHpp2PressureEdgeExitThreshold, "stylus.hpp2.pressure_edge_exit_threshold"},
    {ConfigKeyId::StylusHpp2PressureDeltaNormal, "stylus.hpp2.pressure_delta_normal"},
    {ConfigKeyId::StylusHpp2PressureDeltaTight, "stylus.hpp2.pressure_delta_tight"},
    {ConfigKeyId::StylusHpp2UseTightPressureDelta, "stylus.hpp2.use_tight_pressure_delta"},

    {ConfigKeyId::StylusSpFrameParserEnabled, "stylus.sp.frame_parser_enabled"},
    {ConfigKeyId::StylusSpPeakDetectorEnabled, "stylus.sp.peak_detector_enabled"},
    {ConfigKeyId::StylusSpCoordinateSolverEnabled, "stylus.sp.coordinate_solver_enabled"},
    {ConfigKeyId::StylusSpTiltProcessEnabled, "stylus.sp.tilt_process_enabled"},
    {ConfigKeyId::StylusSpPressureSolverEnabled, "stylus.sp.pressure_solver_enabled"},
    {ConfigKeyId::StylusSpPostPressureEnabled, "stylus.sp.post_pressure_enabled"},
    {ConfigKeyId::StylusSpFakePressureDecreaseEnabled, "stylus.sp.fake_pressure_decrease_enabled"},
    {ConfigKeyId::StylusSpBtFreqShiftDebounceFrames, "stylus.sp.bt_freq_shift_debounce_frames"},
    {ConfigKeyId::StylusSpPressureEdgeEnterThreshold, "stylus.sp.pressure_edge_enter_threshold"},
    {ConfigKeyId::StylusSpPressureEdgeExitThreshold, "stylus.sp.pressure_edge_exit_threshold"},
    {ConfigKeyId::StylusSpTipDownPressureThreshold, "stylus.sp.tip_down_pressure_threshold"},
    {ConfigKeyId::StylusSpBtPressSuppressEnterThreshold, "stylus.sp.bt_press_signal_suppress_enter_threshold"},
    {ConfigKeyId::StylusSpBtPressSuppressExitThreshold, "stylus.sp.bt_press_signal_suppress_exit_threshold"},
    {ConfigKeyId::StylusSpSignalFloor, "stylus.sp.signal_floor"},
    {ConfigKeyId::StylusSpEdgeCoorEnabled, "stylus.sp.edge_coor_enabled"},
    {ConfigKeyId::StylusSpEdgeCoorPostEnabled, "stylus.sp.edge_coor_post_enabled"},
    {ConfigKeyId::StylusSpNoisePostEnabled, "stylus.sp.noise_post_enabled"},
    {ConfigKeyId::StylusSpNoiseSignalRatioThold, "stylus.sp.noise_signal_ratio_thold"},
    {ConfigKeyId::StylusSpNoiseSignalDropRatio, "stylus.sp.noise_signal_drop_ratio"},
    {ConfigKeyId::StylusSpLinearFilterEnabled, "stylus.sp.linear_filter_enabled"},
    {ConfigKeyId::StylusSpCoorReviseEnabled, "stylus.sp.coor_revise_enabled"},
    {ConfigKeyId::StylusSpCoorReviseFactorDim1, "stylus.sp.coor_revise_factor_dim1"},
    {ConfigKeyId::StylusSpCoorReviseFactorDim2, "stylus.sp.coor_revise_factor_dim2"},
    {ConfigKeyId::StylusSpCoorSpeedEnabled, "stylus.sp.coor_speed_enabled"},
    {ConfigKeyId::StylusSpIirFilterEnabled, "stylus.sp.iir_filter_enabled"},
    {ConfigKeyId::StylusSpIirCoefLowInBand, "stylus.sp.iir_coef_low_in_band"},
    {ConfigKeyId::StylusSpIirCoefHighInBand, "stylus.sp.iir_coef_high_in_band"},
    {ConfigKeyId::StylusSpIirSpeedTholdInBand, "stylus.sp.iir_speed_thold_in_band"},
    {ConfigKeyId::StylusSpIirCoefLowEdge, "stylus.sp.iir_coef_low_edge"},
    {ConfigKeyId::StylusSpIirCoefHighEdge, "stylus.sp.iir_coef_high_edge"},
    {ConfigKeyId::StylusSpIirSpeedTholdEdge, "stylus.sp.iir_speed_thold_edge"},
    {ConfigKeyId::StylusSpIirSpeedMax, "stylus.sp.iir_speed_max"},
    {ConfigKeyId::StylusSpIirMaxCoef, "stylus.sp.iir_max_coef"},
    {ConfigKeyId::StylusSpAftCoorEnabled, "stylus.sp.aft_coor_enabled"},
    {ConfigKeyId::StylusSpLockFlashInBandX, "stylus.sp.lock_flash_in_band_x"},
    {ConfigKeyId::StylusSpLockFlashInBandY, "stylus.sp.lock_flash_in_band_y"},
    {ConfigKeyId::StylusSpLockFlashEdgeX, "stylus.sp.lock_flash_edge_x"},
    {ConfigKeyId::StylusSpLockFlashEdgeY, "stylus.sp.lock_flash_edge_y"},
    {ConfigKeyId::StylusSpLockSensorTxCount, "stylus.sp.lock_sensor_tx_count"},
    {ConfigKeyId::StylusSpLockSensorRxCount, "stylus.sp.lock_sensor_rx_count"},
    {ConfigKeyId::StylusSpLockBypass, "stylus.sp.lock_bypass"},
};

std::unordered_map<ConfigKeyId, std::string>& mutableKeyIdToPath()
{
    static std::unordered_map<ConfigKeyId, std::string> map;
    return map;
}

std::unordered_map<std::string, ConfigKeyId>& mutablePathToKeyId()
{
    static std::unordered_map<std::string, ConfigKeyId> map;
    return map;
}

void ensureStaticMapsInitialized()
{
    static const bool initialized = [] {
        auto& idToPath = mutableKeyIdToPath();
        auto& pathToId = mutablePathToKeyId();
        for (const auto& [id, pathView] : kStaticKeyMap) {
            std::string path{pathView};
            idToPath.try_emplace(id, path);
            pathToId.try_emplace(std::move(path), id);
        }
        return true;
    }();
    (void)initialized;
}

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

uint16_t nextAvailableId(uint16_t startInclusive, uint16_t endExclusive)
{
    auto& idToPath = mutableKeyIdToPath();
    for (uint16_t raw = startInclusive; raw < endExclusive; ++raw) {
        if (!idToPath.contains(static_cast<ConfigKeyId>(raw))) {
            return raw;
        }
    }
    return endExclusive;
}

} // namespace

const std::unordered_map<ConfigKeyId, std::string>& keyIdToPath()
{
    ensureStaticMapsInitialized();
    return mutableKeyIdToPath();
}

const std::unordered_map<std::string, ConfigKeyId>& pathToKeyId()
{
    ensureStaticMapsInitialized();
    return mutablePathToKeyId();
}

void registerKeyMapping(ConfigKeyId id, std::string_view yamlPath)
{
    ensureStaticMapsInitialized();

    auto& idToPath = mutableKeyIdToPath();
    auto& pathToId = mutablePathToKeyId();

    if (const auto existing = idToPath.find(id); existing != idToPath.end()) {
        pathToId.erase(existing->second);
    }

    std::string path{yamlPath};
    idToPath[id] = path;
    pathToId[std::move(path)] = id;
}

void registerRuntimeKeyMappings(const ConfigBinder& binder)
{
    ensureStaticMapsInitialized();

    std::vector<std::string> missingTouchPaths;
    std::vector<std::string> missingStylusPaths;
    const auto snapshot = binder.snapshot();
    for (const auto& entry : snapshot.entries) {
        if (tryKeyIdForPath(entry.yamlPath).has_value()) {
            continue;
        }
        if (startsWith(entry.yamlPath, "touch.")) {
            missingTouchPaths.push_back(entry.yamlPath);
        } else if (startsWith(entry.yamlPath, "stylus.")) {
            missingStylusPaths.push_back(entry.yamlPath);
        }
    }

    std::ranges::sort(missingTouchPaths);
    std::ranges::sort(missingStylusPaths);

    uint16_t nextTouch = nextAvailableId(0x0100, 0x0200);
    for (const auto& path : missingTouchPaths) {
        if (nextTouch >= 0x0200) {
            break;
        }
        registerKeyMapping(static_cast<ConfigKeyId>(nextTouch++), path);
        nextTouch = nextAvailableId(nextTouch, 0x0200);
    }

    uint16_t nextStylus = nextAvailableId(0x0200, static_cast<uint16_t>(ConfigKeyId::MaxKeyId));
    for (const auto& path : missingStylusPaths) {
        if (nextStylus >= static_cast<uint16_t>(ConfigKeyId::MaxKeyId)) {
            break;
        }
        registerKeyMapping(static_cast<ConfigKeyId>(nextStylus++), path);
        nextStylus = nextAvailableId(nextStylus, static_cast<uint16_t>(ConfigKeyId::MaxKeyId));
    }
}

std::optional<std::string_view> tryPathForKeyId(ConfigKeyId id)
{
    ensureStaticMapsInitialized();
    const auto& idToPath = mutableKeyIdToPath();
    if (const auto it = idToPath.find(id); it != idToPath.end()) {
        return std::string_view{it->second};
    }
    return std::nullopt;
}

std::optional<ConfigKeyId> tryKeyIdForPath(std::string_view yamlPath)
{
    ensureStaticMapsInitialized();
    const auto& pathToId = mutablePathToKeyId();
    if (const auto it = pathToId.find(std::string{yamlPath}); it != pathToId.end()) {
        return it->second;
    }
    return std::nullopt;
}

ConfigSchemaSnapshot BuildMergedSchema(const ConfigStore& defaults, const ConfigBinder& binder)
{
    ConfigSchemaSnapshot result;
    std::unordered_map<std::string, size_t> indexByPath;

    const auto paths = defaults.allPaths();
    result.entries.reserve(paths.size());
    indexByPath.reserve(paths.size());

    for (const auto& path : paths) {
        ConfigSchemaEntry entry;
        entry.yamlPath = path;
        entry.keyId = tryKeyIdForPath(path).value_or(ConfigKeyId::MaxKeyId);
        entry.defaultValue = defaults.get<ConfigValue>(path);
        entry.currentValue = entry.defaultValue;
        entry.uiType = deriveUiType(entry.defaultValue);
        entry.displayName = deriveDisplayName(path);
        entry.moduleTag = deriveModuleTag(path);
        entry.boundToRuntime = false;

        indexByPath.emplace(entry.yamlPath, result.entries.size());
        result.entries.push_back(std::move(entry));
    }

    auto binderSnapshot = binder.snapshot();
    for (auto& entry : binderSnapshot.entries) {
        if (const auto it = indexByPath.find(entry.yamlPath); it != indexByPath.end()) {
            result.entries[it->second] = std::move(entry);
        } else {
            indexByPath.emplace(entry.yamlPath, result.entries.size());
            result.entries.push_back(std::move(entry));
        }
    }

    std::ranges::sort(result.entries, [](const ConfigSchemaEntry& lhs, const ConfigSchemaEntry& rhs) {
        if (lhs.keyId != rhs.keyId) {
            return static_cast<uint16_t>(lhs.keyId) < static_cast<uint16_t>(rhs.keyId);
        }
        return lhs.yamlPath < rhs.yamlPath;
    });

    return result;
}

} // namespace Config
