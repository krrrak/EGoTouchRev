#include "StylusSolver/StylusPipeline.h"
#include "StylusSolver/hpp2/Hpp2ChargerNoiseProcess.hpp"
#include "StylusSolver/hpp2/Hpp2CmfProcess.hpp"
#include "StylusSolver/hpp2/Hpp2DataQualityProcess.hpp"
#include "StylusSolver/hpp2/Hpp2LinePeakExtractor.hpp"
#include "StylusSolver/hpp2/Hpp2PeakSelector.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using Solvers::HeatmapFrame;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

HeatmapFrame MakeHpp2Frame(int dim1Peak,
                           int dim2Peak,
                           uint16_t dim1PeakValue,
                           uint16_t dim2PeakValue,
                           uint16_t pressure,
                           uint32_t buttonBits = 0,
                           uint32_t auxStatusFlags = 0x1) {
    HeatmapFrame frame{};
    auto& input = frame.stylus.input;
    input.auxStatusFlags = auxStatusFlags; // HPP2 protocol in Auto mode
    input.mainFreq = 0x00b0;
    input.auxFreq = 0x00fc;
    input.framePressure = pressure;
    input.buttonBits = buttonBits;
    input.hpp2LineValid = true;
    input.hpp2LineData.fill(10);
    input.hpp2LineData[static_cast<std::size_t>(dim1Peak)] = dim1PeakValue;
    input.hpp2LineData[static_cast<std::size_t>(60 + dim2Peak)] = dim2PeakValue;
    return frame;
}

Solvers::StylusPenSession MakePenSession(uint8_t stylusId,
                                         Solvers::StylusProtocolHint protocolHint,
                                         uint32_t revision,
                                         bool connected = true) {
    Solvers::StylusPenSession session{};
    session.hasConnectionState = true;
    session.connected = connected;
    session.hasStylusId = connected;
    session.stylusId = connected ? stylusId : 0;
    session.protocolHint = protocolHint;
    session.revision = revision;
    return session;
}

void TestHpp2FrameProducesReport() {
    Solvers::StylusPipeline pipeline;
    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512, 1);

    pipeline.Process(frame);

    Require(!frame.stylus.runtime.Active().flow.terminal, "HPP2 frame should run non-terminal");
    Require(frame.stylus.output.valid, "HPP2 output should be valid");
    Require(frame.stylus.output.inRange, "HPP2 output should be in range");
    Require(frame.stylus.output.tipDown, "HPP2 pressure should produce tip-down");
    Require(frame.stylus.output.pressure == 512, "HPP2 pressure should publish frame pressure");
    Require(frame.stylus.runtime.hpp2.buttonPressed, "HPP2 button bit should be debounced as pressed");
    Require(frame.stylus.output.point.x > 0.0f && frame.stylus.output.point.y > 0.0f,
            "HPP2 coordinate should be populated");
}

void TestHpp2EdgePressureGuardSuppressesPressure() {
    Solvers::StylusPipeline pipeline;
    HeatmapFrame frame = MakeHpp2Frame(0, 7, 1000, 2400, 512);

    pipeline.Process(frame);

    Require(frame.stylus.output.valid, "edge-guarded HPP2 output should still have coordinate");
    Require(frame.stylus.output.inRange, "edge-guarded HPP2 output should remain in range");
    Require(!frame.stylus.output.tipDown, "low edge signal should clear tip-down");
    Require(frame.stylus.output.pressure == 0, "low edge signal should clear pressure");
}

void TestHpp2AbnormalRawRejects() {
    Solvers::StylusPipeline pipeline;

    // Frame 1: warmup to populate line sum history.
    // Without history, energyRatioPrev returns 100 (div-by-zero guard),
    // which is below the 200 threshold required for rawAbnormal.
    HeatmapFrame warmup = MakeHpp2Frame(12, 7, 2600, 2400, 512);
    pipeline.Process(warmup);

    // Frame 2: fill all 100 samples with 1000.
    // rawLineSum = 100 * 1000 = 100000 > 30000 threshold.
    // energyRatioPrev = 100000 * 100 / warmupLineSum ≈ much > 200.
    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512);
    frame.stylus.input.hpp2LineData.fill(1000);

    pipeline.Process(frame);

    Require(frame.stylus.runtime.Active().flow.terminal, "abnormal raw line sum should reject the frame");
    Require(frame.stylus.runtime.hpp2.rawAbnormal, "rawAbnormal flag should be set");
}

void TestHpp2NoPeakRejects() {
    Solvers::StylusPipeline pipeline;
    // Peak values of 10 are well below the 250 signal floor
    HeatmapFrame frame = MakeHpp2Frame(12, 7, 10, 10, 512);

    pipeline.Process(frame);

    Require(frame.stylus.runtime.Active().flow.terminal, "sub-floor peak signal should reject the frame");
}

void TestHpp2ButtonReleaseCounterDecrements() {
    Solvers::StylusPipeline pipeline;

    // Frame 1: button bit set
    HeatmapFrame f1 = MakeHpp2Frame(12, 7, 2600, 2400, 512, 1u);
    pipeline.Process(f1);
    Require(f1.stylus.runtime.hpp2.buttonPressed, "frame 1 button should be pressed");
    Require(f1.stylus.runtime.hpp2.buttonReleaseFrames == 2, "release counter should start at 2");

    // Frame 2: button bit clear, release counter still active
    HeatmapFrame f2 = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u);
    pipeline.Process(f2);
    Require(f2.stylus.runtime.hpp2.buttonPressed, "frame 2 button should still be held by release counter");
    Require(f2.stylus.runtime.hpp2.buttonReleaseFrames == 1, "release counter should decrement to 1");

    // Frame 3: button bit still clear, counter decrements to 0
    HeatmapFrame f3 = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u);
    pipeline.Process(f3);
    Require(f3.stylus.runtime.hpp2.buttonPressed, "frame 3 button should still be held");
    Require(f3.stylus.runtime.hpp2.buttonReleaseFrames == 0, "release counter should reach 0");

    // Frame 4: counter exhausted, button released
    HeatmapFrame f4 = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u);
    pipeline.Process(f4);
    Require(!f4.stylus.runtime.hpp2.buttonPressed, "frame 4 button should be released");
}

void TestForcedHpp2IgnoresAuxFlags() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(frame);

    Require(frame.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "forced HPP2 session should select HPP2 even without aux flags");
    Require(!frame.stylus.runtime.Active().flow.terminal, "forced HPP2 frame should run non-terminal");
    Require(frame.stylus.output.valid, "forced HPP2 output should be valid");
    Require(frame.stylus.output.pressure == 512, "forced HPP2 pressure should be preserved");
}

void TestForcedHpp2WithoutLineDataTerminals() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    HeatmapFrame frame{};
    pipeline.Process(frame);

    Require(frame.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "missing forced HPP2 data should still stay on HPP2");
    Require(frame.stylus.runtime.Active().flow.terminal, "missing forced HPP2 line data should terminal");
    Require(!frame.stylus.output.valid, "missing forced HPP2 line data should not emit output");
}

void TestHpp2HintDoesNotOverrideRawEvidence() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    std::array<uint8_t, 4> shortRaw{};
    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    frame.rawPtr = shortRaw.data();
    frame.rawLen = shortRaw.size();
    pipeline.Process(frame);

    Require(frame.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp3,
            "HPP2 hint must not override concrete raw HPP3 evidence");
    Require(frame.stylus.runtime.Active().flow.terminal,
            "short raw HPP3 evidence should terminal as HPP3 parse failure");
    Require(!frame.stylus.output.valid, "raw evidence path should not emit HPP2 fallback output");
}

void TestHpp3HintDoesNotRunAfterHpp2Fallback() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp3, 1));

    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0x1u);
    pipeline.Process(frame);

    Require(frame.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "HPP3 hint must respect parser HPP2 fallback evidence");
    Require(!frame.stylus.runtime.Active().flow.terminal,
            "HPP2 fallback under HPP3 hint should run HPP2 pipeline");
    Require(frame.stylus.output.valid, "HPP2 fallback under HPP3 hint should emit valid HPP2 output");
}

void TestSameProtocolPenSwitchResetsState() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    HeatmapFrame pressed = MakeHpp2Frame(12, 7, 2600, 2400, 512, 1u, 0u);
    pipeline.Process(pressed);
    Require(pressed.stylus.runtime.hpp2.buttonPressed, "first HPP2 pen should press button");
    Require(pressed.stylus.runtime.hpp2.buttonReleaseFrames == 2,
            "first HPP2 pen should arm release counter");

    pipeline.ApplyPenSession(MakePenSession(4, Solvers::StylusProtocolHint::Hpp2, 2));

    HeatmapFrame released = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(released);
    Require(!released.stylus.runtime.hpp2.buttonPressed,
            "same-protocol pen switch should reset HPP2 button history");
    Require(released.stylus.runtime.hpp2.buttonReleaseFrames == 0,
            "same-protocol pen switch should clear release counter");
}

void TestDisconnectedPenSessionTerminals() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    HeatmapFrame valid = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(valid);
    Require(valid.stylus.output.valid, "connected session should process valid HPP2 data");

    pipeline.ApplyPenSession(MakePenSession(0, Solvers::StylusProtocolHint::Auto, 2, false));

    HeatmapFrame disconnected = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(disconnected);
    Require(disconnected.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "disconnected pen session should retain the previous HPP2 terminal protocol");
    Require(disconnected.stylus.runtime.Active().flow.terminal,
            "disconnected pen session should terminal current frame");
    Require(!disconnected.stylus.output.valid, "disconnected pen session should not emit output");
}

void TestInitialDisconnectedAutoSessionStaysProtocolNeutral() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(0, Solvers::StylusProtocolHint::Auto, 1, false));

    HeatmapFrame disconnected = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(disconnected);

    Require(disconnected.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::None,
            "initial disconnected Auto session should remain protocol-neutral");
    Require(disconnected.stylus.runtime.activeProtocol != Solvers::StylusRuntime::Protocol::Hpp3,
            "initial disconnected Auto session must not default to HPP3");
    Require(disconnected.stylus.runtime.Active().flow.terminal,
            "initial disconnected Auto session should terminal current frame");
    Require(!disconnected.stylus.output.valid,
            "initial disconnected Auto session should not emit output");
}

void TestFreshAutoSessionDoesNotInheritPreviousTerminalProtocol() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(1, Solvers::StylusProtocolHint::Hpp2, 1));

    HeatmapFrame valid = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(valid);
    Require(valid.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "first session should establish HPP2 as last active protocol");
    Require(valid.stylus.output.valid, "first session should process valid HPP2 data");

    pipeline.ApplyPenSession(MakePenSession(4, Solvers::StylusProtocolHint::Auto, 2, true));
    pipeline.ApplyPenSession(MakePenSession(0, Solvers::StylusProtocolHint::Auto, 2, false));

    HeatmapFrame disconnected = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(disconnected);
    Require(disconnected.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::None,
            "fresh Auto session disconnected before parser evidence should stay protocol-neutral");
    Require(disconnected.stylus.runtime.activeProtocol != Solvers::StylusRuntime::Protocol::Hpp2,
            "fresh Auto terminal must not inherit the previous session HPP2 protocol");
    Require(disconnected.stylus.runtime.Active().flow.terminal,
            "fresh Auto disconnected session should terminal current frame");
    Require(!disconnected.stylus.output.valid,
            "fresh Auto disconnected session should not emit output");
}

void TestInitialDisconnectedHpp2HintSelectsHpp2Terminal() {
    Solvers::StylusPipeline pipeline;
    pipeline.ApplyPenSession(MakePenSession(0, Solvers::StylusProtocolHint::Hpp2, 1, false));

    HeatmapFrame disconnected = MakeHpp2Frame(12, 7, 2600, 2400, 512, 0u, 0u);
    pipeline.Process(disconnected);

    Require(disconnected.stylus.runtime.activeProtocol == Solvers::StylusRuntime::Protocol::Hpp2,
            "initial disconnected HPP2 hint should select HPP2 terminal protocol");
    Require(disconnected.stylus.runtime.Active().flow.terminal,
            "initial disconnected HPP2 hint should terminal current frame");
    Require(!disconnected.stylus.output.valid,
            "initial disconnected HPP2 hint should not emit output");
}

void FillCmfInput(Solvers::Stylus::Hpp2::Runtime& runtime) {
    runtime.line.raw.fill(0);
    const std::array<uint16_t, 5> dim1{{10, 20, 100, 20, 10}};
    const std::array<uint16_t, 4> dim2{{7, 70, 7, 9}};
    for (std::size_t i = 0; i < dim1.size(); ++i) {
        runtime.line.raw[i] = dim1[i];
    }
    for (std::size_t i = 0; i < dim2.size(); ++i) {
        runtime.line.raw[5 + i] = dim2[i];
    }
}

Solvers::Stylus::Hpp2::Settings MakeCmfSettings() {
    Solvers::Stylus::Hpp2::Settings settings{};
    settings.sensorTxCount = 5;
    settings.sensorRxCount = 4;
    settings.cmfWindowRadius = 1;
    return settings;
}

void TestHpp2CmfPublishesMaxAndHistoryHead() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeCmfSettings();
    FillCmfInput(runtime);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2CmfProcess{}.Process(ctx);

    Require(runtime.line.cmnBaseline[0] == 10 && runtime.line.cmnBaseline[1] == 20 &&
                runtime.line.cmnBaseline[2] == 20 && runtime.line.cmnBaseline[3] == 20 &&
                runtime.line.cmnBaseline[4] == 10,
            "CMF group0 baseline should match sliding min-max result");
    Require(runtime.line.cmnSubtracted[2] == 80, "CMF group0 should subtract baseline from raw peak");
    Require(state.m_cmnSum[0] == 80, "CMF group0 sum should be published");
    Require(state.m_cmnMax[0] == 20, "CMF group0 max should be published");
    Require(state.m_cmnMin[0] == 10, "CMF group0 min should be published");
    Require(state.m_cmnSumHistory[0][0] == state.m_cmnSum[0], "CMF sum history head should match current sum");
    Require(state.m_cmnMaxHistory[0][0] == state.m_cmnMax[0], "CMF max history head should match current max");
    Require(state.m_cmnMinHistory[0][0] == state.m_cmnMin[0], "CMF min history head should match current min");
    Require(state.m_cmnSum[1] == 28 && state.m_cmnMax[1] == 7 && state.m_cmnMin[1] == 7,
            "CMF group1 statistics should use group-local length and offset");
}

void TestHpp2CmfRotatesHistory() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeCmfSettings();
    FillCmfInput(runtime);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2CmfProcess cmf;
    cmf.Process(ctx);
    const uint32_t firstSum = state.m_cmnSum[0];
    const uint16_t firstMax = state.m_cmnMax[0];
    const uint16_t firstMin = state.m_cmnMin[0];

    runtime.line.raw.fill(50);
    cmf.Process(ctx);

    Require(state.m_cmnSumHistory[0][0] == 250, "CMF latest sum should be at history head");
    Require(state.m_cmnMaxHistory[0][0] == 50, "CMF latest max should be at history head");
    Require(state.m_cmnMinHistory[0][0] == 50, "CMF latest min should be at history head");
    Require(state.m_cmnSumHistory[0][1] == firstSum, "CMF previous sum should rotate to history slot 1");
    Require(state.m_cmnMaxHistory[0][1] == firstMax, "CMF previous max should rotate to history slot 1");
    Require(state.m_cmnMinHistory[0][1] == firstMin, "CMF previous min should rotate to history slot 1");
}

void TestHpp2CmfRangeGateDisabledByDefault() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeCmfSettings();
    settings.cmnRangeStart[0] = 0;
    settings.cmnRangeEnd[0] = 4;
    FillCmfInput(runtime);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2CmfProcess{}.Process(ctx);

    Require(state.m_cmnRangeSumHistory[0][0] == 0,
            "CMF range sum should stay zero when groupParam gate is disabled");
}

void TestHpp2CmfRangeGateSumsSubtractedLine() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeCmfSettings();
    settings.cmnRangeSumEnabled[0] = 1;
    settings.cmnRangeStart[0] = 1;
    settings.cmnRangeEnd[0] = 3;
    settings.cmnRangeSumEnabled[1] = 1;
    settings.cmnRangeStart[1] = 1;
    settings.cmnRangeEnd[1] = 3;
    FillCmfInput(runtime);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2CmfProcess{}.Process(ctx);

    Require(state.m_cmnRangeSumHistory[0][0] == 80,
            "CMF group0 range sum should accumulate subtracted line values");
    Require(state.m_cmnRangeSumHistory[1][0] == 65,
            "CMF group1 range sum should use group-local range with group offset");
}

Solvers::Stylus::Hpp2::Settings MakeChargerNoiseSettings() {
    Solvers::Stylus::Hpp2::Settings settings{};
    settings.sensorTxCount = 7;
    settings.sensorRxCount = 4;
    settings.chargerNoisePeakProtectRadius = 1;
    settings.chargerNoiseClearFloor = 1;
    settings.chargerNoiseRatioThreshold = 100;
    settings.chargerNoiseMinRawSample = 50;
    return settings;
}

void PrepareChargerNoiseInput(Solvers::Stylus::Hpp2::Runtime& runtime,
                              Solvers::Stylus::Hpp2::State& state,
                              std::size_t freqIdx,
                              int noisyIndex) {
    runtime.line.raw.fill(10);
    runtime.line.cmnSubtracted.fill(0);
    runtime.line.raw[static_cast<std::size_t>(noisyIndex)] = 1000;
    runtime.line.cmnSubtracted[static_cast<std::size_t>(noisyIndex)] = 1000;
    state.m_rawHistory[freqIdx][0].fill(10);
}

void TestHpp2ChargerNoiseProtectsPreviousBoundary() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeChargerNoiseSettings();
    state.m_curFreqIdx = 0;
    state.m_f1FrameCnt = 2;
    state.m_prevPeakDim1ByFreq[0] = 2;
    state.m_prevPeakBoundaryDim1ByFreq[0] = Solvers::Stylus::Hpp2::PeakBoundary{1, 3, true};
    PrepareChargerNoiseInput(runtime, state, 0, 4);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2ChargerNoiseProcess{}.Process(ctx);

    Require(state.m_noiseSum[0] == 0,
            "previous boundary plus radius should protect sample outside previous center radius");
}

void TestHpp2ChargerNoiseLeavesOutsideBoundaryCandidate() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeChargerNoiseSettings();
    state.m_curFreqIdx = 0;
    state.m_f1FrameCnt = 2;
    state.m_prevPeakDim1ByFreq[0] = 2;
    state.m_prevPeakBoundaryDim1ByFreq[0] = Solvers::Stylus::Hpp2::PeakBoundary{1, 3, true};
    PrepareChargerNoiseInput(runtime, state, 0, 5);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2ChargerNoiseProcess{}.Process(ctx);

    Require(state.m_noiseSum[0] == 1000,
            "sample outside previous boundary plus radius should remain charger-noise candidate");
}

void TestHpp2ChargerNoisePreviousBoundaryIsFrequencyLocal() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeChargerNoiseSettings();
    state.m_curFreqIdx = 1;
    state.m_f2FrameCnt = 2;
    state.m_prevPeakDim1ByFreq[0] = 2;
    state.m_prevPeakBoundaryDim1ByFreq[0] = Solvers::Stylus::Hpp2::PeakBoundary{1, 3, true};
    PrepareChargerNoiseInput(runtime, state, 1, 4);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2ChargerNoiseProcess{}.Process(ctx);

    Require(state.m_noiseSum[1] == 1000,
            "F1 previous boundary should not protect an F2 charger-noise frame");
}

void TestHpp2ChargerNoiseInvalidIndexKeepsRatioZero() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakeChargerNoiseSettings();
    state.m_curFreqIdx = 0;
    state.m_f1FrameCnt = 2;
    state.m_prevPeakBoundaryDim1ByFreq[0] = Solvers::Stylus::Hpp2::PeakBoundary{1, 3, true};
    PrepareChargerNoiseInput(runtime, state, 0, 4);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2ChargerNoiseProcess{}.Process(ctx);

    Require(runtime.line.chargerNoiseRatio[4] == 0,
            "TSACore leaves charger-noise ratio zero for protected peak-boundary indexes");
}

void TestHpp2DataQualityDoesNotLatchChargerNoiseFlag() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    Solvers::Stylus::Hpp2::Settings settings{};
    state.m_curFreqIdx = 0;
    state.m_noiseFlag[0] = 1;

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2DataQualityProcess{}.UpdateFrequencyLatch(ctx);

    Require(!state.m_freqNoiseLatchF1,
            "ChargerNoiseJudge history flag should not directly become a frequency bypass latch");
}

Solvers::Stylus::Hpp2::Settings MakePeakExtractorSettings() {
    Solvers::Stylus::Hpp2::Settings settings{};
    settings.sensorTxCount = 5;
    settings.sensorRxCount = 3;
    settings.peakSignalFloor = 100;
    settings.peakNetSignalFloor = 200;
    settings.peakMinWidth = 1;
    settings.peakMaxWidth = 10;
    return settings;
}

void FillLowNetPeak(Solvers::Stylus::Hpp2::Runtime& runtime) {
    runtime.line.cmnSubtracted.fill(0);
    runtime.line.cmnBaseline.fill(0);
    runtime.line.cmnSubtracted[0] = 290;
    runtime.line.cmnSubtracted[1] = 300;
    runtime.line.cmnSubtracted[2] = 301;
    runtime.line.cmnSubtracted[3] = 300;
    runtime.line.cmnSubtracted[4] = 290;
}

void TestHpp2PeakExtractorUsesIndependentNetThreshold() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakePeakExtractorSettings();
    FillLowNetPeak(runtime);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2LinePeakExtractor{}.Process(ctx);

    Require(state.m_peakCountDim1 == 0,
            "TX1 peak extractor should reject candidates below the independent net-signal threshold");

    settings.peakNetSignalFloor = 20;
    state.m_peakTableDim1 = {};
    state.m_peakCountDim1 = 0;
    Solvers::Stylus::Hpp2::Context acceptedCtx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2LinePeakExtractor{}.Process(acceptedCtx);

    Require(state.m_peakCountDim1 == 1,
            "TX1 peak extractor should keep local peaks when only the net-signal threshold is lowered");
    Require(state.m_peakTableDim1[0].peakSignal == 11,
            "UpdatePeakPrpt should compute peak signal from search-profile peak minus region minimum");
}

Solvers::Stylus::Hpp2::PeakUnit MakeSelectedPeakUnit(int noiseProp, bool avgHighAbnormal) {
    Solvers::Stylus::Hpp2::PeakUnit unit{};
    unit.valid = true;
    unit.index = 2;
    unit.leftBoundary = 1;
    unit.rightBoundary = 3;
    unit.peakSignal = 600;
    unit.netSignal = 800;
    unit.candidateCoor = 0x400;
    unit.noiseProp = noiseProp;
    unit.avgHighAbnormal = avgHighAbnormal;
    return unit;
}

void TestHpp2PeakSelectorIgnoresNonAvgHighNoiseFlagsForBypass() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakePeakExtractorSettings();
    runtime.mainFreq = Solvers::Stylus::Hpp2::kFreqF1;
    state.m_peakCountDim1 = 1;
    state.m_peakTableDim1[0] = MakeSelectedPeakUnit(0x02, false);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2PeakSelector{}.Process(ctx);

    Require(!runtime.bypassCurFrame,
            "selected non-avg-high noise flag should not bypass the current frame");
    Require(runtime.selectedPeakDim1 == 2,
            "selected peak should still be published when avg-high abnormal is clear");
}

void TestHpp2PeakSelectorBypassesAvgHighSelectedPeak() {
    HeatmapFrame frame{};
    Solvers::Stylus::Hpp2::Runtime runtime{};
    Solvers::Stylus::Hpp2::State state{};
    auto settings = MakePeakExtractorSettings();
    runtime.mainFreq = Solvers::Stylus::Hpp2::kFreqF1;
    state.m_peakCountDim1 = 1;
    state.m_peakTableDim1[0] = MakeSelectedPeakUnit(0x01, true);

    Solvers::Stylus::Hpp2::Context ctx{frame, runtime, settings, state};
    Solvers::Stylus::Hpp2::Hpp2PeakSelector{}.Process(ctx);

    Require(runtime.bypassCurFrame,
            "selected avg-high abnormal flag should bypass the current frame");
    Require(state.m_bypassCounter == 1,
            "avg-high selected bypass should increment the bypass counter");
}

} // namespace

int main() {
    try {
        TestHpp2FrameProducesReport();
        TestHpp2EdgePressureGuardSuppressesPressure();
        TestHpp2AbnormalRawRejects();
        TestHpp2NoPeakRejects();
        TestHpp2ButtonReleaseCounterDecrements();
        TestForcedHpp2IgnoresAuxFlags();
        TestForcedHpp2WithoutLineDataTerminals();
        TestHpp2HintDoesNotOverrideRawEvidence();
        TestHpp3HintDoesNotRunAfterHpp2Fallback();
        TestSameProtocolPenSwitchResetsState();
        TestDisconnectedPenSessionTerminals();
        TestInitialDisconnectedAutoSessionStaysProtocolNeutral();
        TestFreshAutoSessionDoesNotInheritPreviousTerminalProtocol();
        TestInitialDisconnectedHpp2HintSelectsHpp2Terminal();
        TestHpp2CmfPublishesMaxAndHistoryHead();
        TestHpp2CmfRotatesHistory();
        TestHpp2CmfRangeGateDisabledByDefault();
        TestHpp2CmfRangeGateSumsSubtractedLine();
        TestHpp2ChargerNoiseProtectsPreviousBoundary();
        TestHpp2ChargerNoiseLeavesOutsideBoundaryCandidate();
        TestHpp2ChargerNoisePreviousBoundaryIsFrequencyLocal();
        TestHpp2ChargerNoiseInvalidIndexKeepsRatioZero();
        TestHpp2DataQualityDoesNotLatchChargerNoiseFlag();
        TestHpp2PeakExtractorUsesIndependentNetThreshold();
        TestHpp2PeakSelectorIgnoresNonAvgHighNoiseFlagsForBypass();
        TestHpp2PeakSelectorBypassesAvgHighSelectedPeak();
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] StylusHpp2PipelineTest: " << ex.what() << "\n";
        return 1;
    }
    std::cout << "[PASS] StylusHpp2PipelineTest\n";
    return 0;
}
