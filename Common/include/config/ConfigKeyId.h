#pragma once

#include <cstdint>

namespace Config {

// ConfigKeyId: 紧凑的 16-bit 配置键 ID。
// 布局: high byte = 模块分区, low byte = 模块内序号
//
//   Service:    0x00xx
//   Touch:      0x01xx
//   Stylus:     0x02xx
//   Reserved:   0x03xx

enum class ConfigKeyId : uint16_t {
    // ── Service (0x0000-0x00FF) ──
    SvcMode              = 0x0000,
    SvcAutoMode          = 0x0001,
    SvcStylusVhfEnabled  = 0x0002,
    SvcPenButtonMode     = 0x0003,
    SvcPenButtonRoute    = 0x0004,

    // ── Touch (0x0100-0x01FF) ──
    TouchBaselineBgAlphaShift     = 0x0100,
    TouchBaselineBgMaxStep        = 0x0101,
    TouchBaselineNoFingerAlphaShift = 0x0102,
    TouchBaselineNoFingerMaxStep  = 0x0103,
    TouchBaselineRecoveryAlphaShift = 0x0104,
    TouchBaselineRecoveryMaxFrames  = 0x0105,
    TouchBaselineRecoveryMaxStep  = 0x0106,
    TouchFrameParserEnabled       = 0x0107,

    // ── Stylus: HPP2 (0x0200-0x022F) ──
    StylusHpp2Enabled                     = 0x0200,
    StylusHpp2SensorTxCount               = 0x0201,
    StylusHpp2SensorRxCount               = 0x0202,
    StylusHpp2CmfWindowRadius             = 0x0203,
    StylusHpp2RawAbnormalLineSumThreshold = 0x0204,
    StylusHpp2RawAbnormalEnergyRatioThreshold = 0x0205,
    StylusHpp2CmnAbnormalSumThreshold     = 0x0206,
    StylusHpp2CmnAbnormalMinThreshold     = 0x0207,
    StylusHpp2ChargerNoiseClearFloor      = 0x0208,
    StylusHpp2ChargerNoiseRatioThreshold  = 0x0209,
    StylusHpp2ChargerNoiseSumThreshold    = 0x020A,
    StylusHpp2ChargerNoiseMaxSampleThreshold  = 0x020B,
    StylusHpp2ChargerNoiseAbnormalChannelThreshold = 0x020C,
    StylusHpp2ChargerNoisePeakProtectRadius = 0x020D,
    StylusHpp2ChargerNoiseMinRawSample    = 0x020E,
    StylusHpp2PeakSignalFloor             = 0x020F,
    StylusHpp2PeakSearchNeighborDist      = 0x0210,
    StylusHpp2PeakMinWidth                = 0x0211,
    StylusHpp2PeakMaxWidth                = 0x0212,
    StylusHpp2PressureEdgeEnterThreshold  = 0x0213,
    StylusHpp2PressureEdgeExitThreshold   = 0x0214,
    StylusHpp2PressureDeltaNormal         = 0x0215,
    StylusHpp2PressureDeltaTight          = 0x0216,
    StylusHpp2UseTightPressureDelta       = 0x0217,

    // ── Stylus: SP (0x0230-0x027F) ──
    StylusSpFrameParserEnabled            = 0x0230,
    StylusSpPeakDetectorEnabled           = 0x0231,
    StylusSpCoordinateSolverEnabled       = 0x0232,
    StylusSpTiltProcessEnabled            = 0x0233,
    StylusSpPressureSolverEnabled         = 0x0234,
    StylusSpPostPressureEnabled           = 0x0235,
    StylusSpFakePressureDecreaseEnabled   = 0x0236,
    StylusSpBtFreqShiftDebounceFrames     = 0x0237,
    StylusSpPressureEdgeEnterThreshold    = 0x0238,
    StylusSpPressureEdgeExitThreshold     = 0x0239,
    StylusSpTipDownPressureThreshold      = 0x023A,
    StylusSpBtPressSuppressEnterThreshold = 0x023B,
    StylusSpBtPressSuppressExitThreshold  = 0x023C,
    StylusSpSignalFloor                   = 0x023D,
    StylusSpEdgeCoorEnabled               = 0x023E,
    StylusSpEdgeCoorPostEnabled           = 0x023F,
    StylusSpNoisePostEnabled              = 0x0240,
    StylusSpNoiseSignalRatioThold         = 0x0241,
    StylusSpNoiseSignalDropRatio          = 0x0242,
    StylusSpLinearFilterEnabled           = 0x0243,
    StylusSpCoorReviseEnabled             = 0x0244,
    StylusSpCoorReviseFactorDim1          = 0x0245,
    StylusSpCoorReviseFactorDim2          = 0x0246,
    StylusSpCoorSpeedEnabled              = 0x0247,
    StylusSpIirFilterEnabled              = 0x0248,
    StylusSpIirCoefLowInBand              = 0x0249,
    StylusSpIirCoefHighInBand             = 0x024A,
    StylusSpIirSpeedTholdInBand           = 0x024B,
    StylusSpIirCoefLowEdge                = 0x024C,
    StylusSpIirCoefHighEdge               = 0x024D,
    StylusSpIirSpeedTholdEdge             = 0x024E,
    StylusSpIirSpeedMax                   = 0x024F,
    StylusSpIirMaxCoef                    = 0x0250,
    StylusSpAftCoorEnabled                = 0x0251,
    StylusSpLockFlashInBandX              = 0x0252,
    StylusSpLockFlashInBandY              = 0x0253,
    StylusSpLockFlashEdgeX                = 0x0254,
    StylusSpLockFlashEdgeY                = 0x0255,
    StylusSpLockSensorTxCount             = 0x0256,
    StylusSpLockSensorRxCount             = 0x0257,
    StylusSpLockBypass                    = 0x0258,

    // ── Sentinel ──
    MaxKeyId = 0x0300,
};

} // namespace Config
