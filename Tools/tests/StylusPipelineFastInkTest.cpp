#include "StylusSolver/BtPressBuffer.hpp"
#include "StylusSolver/GridPeakDetector.hpp"
#include "StylusSolver/PressureSolver.hpp"
#include "StylusSolver/StylusPipeline.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Asa::EdgeSignalInputs;
using Asa::GridPeakDetector;
using Asa::PressureSolver;
using Solvers::HeatmapFrame;
using Solvers::StylusFrameData;
using Solvers::StylusPipeline;

constexpr size_t kMasterBytes = 5063;
constexpr size_t kSlaveHeaderBytes = 7;
constexpr size_t kSlaveWordCount = 166;
constexpr size_t kSlaveFrameBytes = kSlaveHeaderBytes + kSlaveWordCount * 2;
constexpr int kGridDim = 9;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::array<uint16_t, kGridDim * kGridDim> MakeCrossGrid(
    uint16_t center,
    uint16_t nearAxis,
    uint16_t diag,
    uint16_t farAxis,
    int peakRow = 4,
    int peakCol = 4) {
    std::array<uint16_t, kGridDim * kGridDim> grid{};
    auto set = [&](int r, int c, uint16_t v) {
        if (r >= 0 && r < kGridDim && c >= 0 && c < kGridDim) {
            grid[static_cast<size_t>(r * kGridDim + c)] = v;
        }
    };

    set(peakRow, peakCol, center);
    set(peakRow - 1, peakCol, nearAxis);
    set(peakRow + 1, peakCol, nearAxis);
    set(peakRow, peakCol - 1, nearAxis);
    set(peakRow, peakCol + 1, nearAxis);
    set(peakRow - 1, peakCol - 1, diag);
    set(peakRow - 1, peakCol + 1, diag);
    set(peakRow + 1, peakCol - 1, diag);
    set(peakRow + 1, peakCol + 1, diag);
    set(peakRow - 2, peakCol, farAxis);
    set(peakRow + 2, peakCol, farAxis);
    set(peakRow, peakCol - 2, farAxis);
    set(peakRow, peakCol + 2, farAxis);
    return grid;
}

std::vector<uint8_t> BuildCombinedStylusFrame(
    uint16_t tx1AnchorRow,
    uint16_t tx1AnchorCol,
    const std::array<uint16_t, kGridDim * kGridDim>& tx1Grid,
    uint16_t tx2AnchorRow = 0,
    uint16_t tx2AnchorCol = 0,
    const std::array<uint16_t, kGridDim * kGridDim>& tx2Grid = {}) {
    std::vector<uint8_t> raw(kMasterBytes + kSlaveFrameBytes, 0);
    auto writeWord = [&](size_t wordIndex, uint16_t value) {
        const size_t off = kMasterBytes + kSlaveHeaderBytes + wordIndex * 2;
        raw[off] = static_cast<uint8_t>(value & 0xFFu);
        raw[off + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    };

    writeWord(0, tx1AnchorRow);
    writeWord(1, tx1AnchorCol);
    for (size_t i = 0; i < tx1Grid.size(); ++i) {
        writeWord(2 + i, tx1Grid[i]);
    }

    writeWord(83, tx2AnchorRow);
    writeWord(84, tx2AnchorCol);
    for (size_t i = 0; i < tx2Grid.size(); ++i) {
        writeWord(85 + i, tx2Grid[i]);
    }
    return raw;
}

HeatmapFrame RunFrame(StylusPipeline& pipeline,
                      const std::vector<uint8_t>& raw,
                      uint16_t btPressure,
                      uint64_t timestamp = 0,
                      bool pushBtPressure = true) {
    HeatmapFrame frame{};
    frame.rawPtr = raw.data();
    frame.rawLen = raw.size();
    frame.timestamp = timestamp;
    if (pushBtPressure) {
        pipeline.SetBtMcuPressure(btPressure);
    }
    pipeline.Process(frame);
    return frame;
}

void LoadFromSavedText(StylusPipeline& pipeline, const std::string& saved) {
    std::istringstream in(saved);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        pipeline.LoadConfig(line.substr(0, eq), line.substr(eq + 1));
    }
}

// The pipeline still finalizes through legacy mirrors in this rollout. These
// tests normalize the final stylus frame into the target contract before
// asserting on input/output/interop semantics.
StylusFrameData ContractView(const HeatmapFrame& frame) {
    StylusFrameData stylus = frame.stylus;
    stylus.SyncContractFromLegacyFields();
    return stylus;
}

void TestPressureStageSignalSuppressHysteresis() {
    PressureSolver solver;
    solver.signalSuppressEnabled = true;
    solver.signalSuppressEnter = 2200;
    solver.signalSuppressExit = 3200;
    solver.edgeSignalSuppressEnabled = false;

    const auto s0 = solver.SolveStage(180, true, 1800, false);
    Require(s0.mappedPressure > 0, "mapped pressure should be produced");
    Require(s0.realPressure > 0, "real pressure should be produced");
    Require(!s0.signalSuppressActive, "signal suppression should stay disabled");

    const auto s1 = solver.SolveStage(180, true, 2500, false);
    Require(s1.realPressure > 0, "real pressure should remain unsuppressed");
    Require(!s1.signalSuppressActive, "signal suppression should remain disabled");

    const auto s2 = solver.SolveStage(180, true, 3500, false);
    Require(s2.realPressure > 0, "pressure should remain valid");
    Require(!s2.signalSuppressActive, "suppression should stay disabled");
}

void TestPressureStageEdgeSuppressHysteresis() {
    PressureSolver solver;
    solver.signalSuppressEnabled = false;
    solver.edgeSignalSuppressEnabled = true;
    solver.edgeSignalSuppressEnter = 1500;
    solver.edgeSignalSuppressExit = 3000;

    EdgeSignalInputs edge{};
    edge.dim1Active = true;
    edge.dim1Signal = 1400;
    const auto s0 = solver.SolveStage(180, true, 5000, true, edge);
    Require(s0.realPressure > 0, "edge path should no longer suppress pressure");
    Require(!s0.edgeSignalSuppressActive, "edge suppression should stay disabled");

    edge.dim1Signal = 1800;
    const auto s1 = solver.SolveStage(180, true, 5000, true, edge);
    Require(s1.realPressure >= 0, "edge path should remain valid");

    edge.dim1Signal = 3201;
    const auto s2 = solver.SolveStage(180, true, 5000, true, edge);
    Require(s2.realPressure >= 0, "edge pressure should remain valid");
    Require(!s2.edgeSignalSuppressActive, "edge suppression should stay disabled");
}

void TestPressureStagePredictsBetweenRealSamples() {
    PressureSolver solver;
    Asa::BtPressureSample real1{200, 1, true};
    const auto s1 = solver.SolveStage(real1, true);
    Require(s1.isRealMeasurement, "first sample should be marked real");
    Require(s1.btSeq == 1, "first sample should carry seq 1");
    Require(s1.predictedAgeFrames == 0, "real sample should reset predicted age");

    const auto s2 = solver.SolveStage(real1, true);
    Require(!s2.isRealMeasurement, "reused snapshot should become predicted frame");
    Require(s2.btSeq == 1, "predicted frame should keep last seq");
    Require(s2.predictedAgeFrames == 1, "predicted age should increment");

    Asa::BtPressureSample real2{260, 2, true};
    const auto s3 = solver.SolveStage(real2, true);
    Require(s3.isRealMeasurement, "new seq should become real measurement");
    Require(s3.btSeq == 2, "new real sample should update seq");
    Require(s3.predictedAgeFrames == 0, "new real sample should clear predicted age");
    Require(solver.GetHistoryCount() >= 3, "solver should retain short pressure history");
}

void TestPressureStageRealZeroDropsImmediately() {
    PressureSolver solver;
    const auto down = solver.SolveStage(Asa::BtPressureSample{240, 1, true}, true);
    Require(down.realPressure > 0, "non-zero real sample should produce pressure");

    const auto up = solver.SolveStage(Asa::BtPressureSample{0, 2, true}, true);
    Require(up.isRealMeasurement, "zero sample with new seq should be real");
    Require(up.realPressure == 0, "real zero sample should force immediate zero output");
    Require(up.predictedAgeFrames == 0, "real zero sample should not be treated as predicted");
}

void TestBtPressBufferDefaultMode() {
    Asa::BtPressBuffer buf;
    Require(buf.Resolve(0, 0) == 0, "empty buffer should return zero");
    buf.Push(100);
    buf.Push(200);
    buf.Push(300);
    buf.Push(400);
    Require(buf.Resolve(0, 0) == 400, "default mode should return latest slot");
    Require(buf.Resolve(5, 0) == 400, "default mode ignores frame count");
}

void TestBtPressBufferOnCellMapping() {
    Asa::BtPressBuffer buf;
    buf.Push(10);
    buf.Push(20);
    buf.Push(30);
    buf.Push(40);
    Require(buf.Resolve(0, 1) == 40, "OnCell mode should return latest slot");
    Require(buf.Resolve(1, 1) == 40, "OnCell mode should ignore frame mapping");
    Require(buf.Resolve(5, 1) == 40, "OnCell mode should always return latest slot");
    Require(buf.Resolve(6, 1) == 40, "OnCell mode should remain latest slot");
}

void TestBtPressBufferInCellMapping() {
    Asa::BtPressBuffer buf;
    buf.Push(10);
    buf.Push(20);
    buf.Push(30);
    buf.Push(40);
    Require(buf.Resolve(0, 2) == 40, "InCell mode should return latest slot");
    Require(buf.Resolve(1, 2) == 40, "InCell mode should ignore frame mapping");
    Require(buf.Resolve(2, 2) == 40, "InCell mode should stay latest slot");
    Require(buf.Resolve(3, 2) == 40, "InCell mode should stay latest slot");
    Require(buf.Resolve(4, 2) == 40, "InCell mode should remain latest slot");
}

void TestBtPressBufferReset() {
    Asa::BtPressBuffer buf;
    buf.Push(100);
    Require(buf.Resolve(0, 0) == 100, "push should store value");
    buf.Reset();
    Require(buf.Resolve(0, 0) == 0, "reset should clear buffer");
}

void TestBtPressBufferSeqSnapshot() {
    Asa::BtPressBuffer buf;
    const auto empty = buf.ReadLatest();
    Require(!empty.hasSample, "empty snapshot should report no sample");

    buf.Push(111);
    const auto s1 = buf.ReadLatest();
    Require(s1.hasSample, "snapshot should report sample after push");
    Require(s1.pressure == 111, "snapshot should carry pushed pressure");
    Require(s1.seq == 1, "first push should advance seq to 1");

    buf.Push(222);
    const auto s2 = buf.ReadLatest();
    Require(s2.pressure == 222, "latest snapshot should track latest pressure");
    Require(s2.seq == 2, "second push should advance seq to 2");
}

void TestStylusPipelineValidFrameProjectsInputOutputAndInteropContract() {
    StylusPipeline pipeline;

    const auto tx1Grid = MakeCrossGrid(16000, 14000, 12000, 10000);
    const auto tx2Grid = MakeCrossGrid(6000, 5000, 4000, 3000);
    const auto raw = BuildCombinedStylusFrame(10, 10, tx1Grid, 10, 10, tx2Grid);

    const auto frame = RunFrame(pipeline, raw, 300, 8);
    const auto stylus = ContractView(frame);

    Require(stylus.input.slaveValid, "valid frame should report slave-valid input");
    Require(stylus.input.tx1BlockValid, "valid frame should report tx1-valid input");
    Require(stylus.input.tx2BlockValid, "valid frame should report tx2-valid input");
    Require(stylus.input.btSample.hasSample, "valid frame should snapshot BT input");
    Require(stylus.input.btSample.pressure == 300,
            "valid frame should snapshot raw BT pressure");
    Require(stylus.input.btSample.seq == 1,
            "first BT push should be visible in the input snapshot");
    Require(stylus.output.valid, "valid frame should project a valid output");
    Require(stylus.output.inRange, "valid frame should project in-range output");
    Require(stylus.output.tipDown, "valid frame should project tip-down output");
    Require(stylus.output.pressure > 0, "valid frame should output pressure");
    Require(stylus.output.point.valid, "valid frame should keep a valid output point");
    Require(stylus.output.point.pressure == stylus.output.pressure,
            "output point pressure should match output pressure");
    Require(stylus.interop.signalX == 16000,
            "contract interop should expose TX1 peak signal");
    Require(stylus.interop.signalY == 6000,
            "contract interop should expose TX2 peak signal");
    Require(stylus.interop.maxRawPeak == 16000,
            "contract interop should expose max raw peak");
    Require(stylus.output.point.peakTx1 == 14666,
            "contract output should keep TX1 composite peak");
    Require(stylus.output.point.peakTx2 == 6000,
            "contract output should keep TX2 composite peak");
#if EGOTOUCH_DIAG
    Require(stylus.debug.parse.slaveValid,
            "debug parse snapshot should track valid slave input");
    Require(stylus.debug.coord.pressureIsReal,
            "first pushed pressure should be marked real in diagnostics");
    Require(stylus.debug.coord.btSeq == 1,
            "debug diagnostics should keep the first BT sequence");
#endif
}

void TestStylusPipelineWeakSignalKeepsSolvedPointButStaysOutOfTipDown() {
    StylusPipeline pipeline;

    const auto weakGrid = MakeCrossGrid(1800, 1400, 900, 600);
    const auto raw = BuildCombinedStylusFrame(12, 12, weakGrid, 12, 12, {});

    const auto frame = RunFrame(pipeline, raw, 180, 8);
    const auto stylus = ContractView(frame);

    Require(stylus.output.valid, "weak-signal frame should still solve a point");
    Require(stylus.output.inRange, "weak-signal frame should stay in range");
    Require(!stylus.output.tipDown,
            "weak-signal frame should not project writing state");
    Require(stylus.output.pressure == 0,
            "weak-signal frame should keep pressure closed");
    Require(stylus.output.point.valid,
            "weak-signal frame should keep the solved point in output");
    Require(stylus.output.point.pressure == 0,
            "weak-signal frame should normalize output point pressure to zero");
    Require(stylus.input.btSample.pressure == 180,
            "weak-signal frame should still snapshot BT input");
}

void TestStylusPipelinePredictedPressureProjectsBtInputContract() {
    StylusPipeline pipeline;
    pipeline.LoadConfig("sp.filterMode", "2");

    const auto raw = BuildCombinedStylusFrame(
        10,
        10,
        MakeCrossGrid(12000, 9000, 7000, 5000, 4, 4),
        10,
        10,
        {});

    const auto downFrame = RunFrame(pipeline, raw, 220, 8, true);
    const auto down = ContractView(downFrame);
    Require(down.output.valid, "first frame should project valid output");
    Require(down.output.pressure > 0, "first frame should output pressure");
    Require(down.output.tipDown, "first frame should project tip-down output");
    Require(down.input.btSample.seq == 1,
            "first frame should expose the first BT sequence");

    const auto predictedFrame = RunFrame(pipeline, raw, 220, 16, false);
    const auto predicted = ContractView(predictedFrame);
    Require(predicted.output.valid, "predicted frame should stay valid");
    Require(predicted.output.pressure > 0,
            "predicted frame should keep pressure output");
    Require(predicted.output.tipDown,
            "predicted frame should stay in writing output state");
    Require(predicted.input.btSample.hasSample,
            "predicted frame should keep the snapped BT sample visible");
    Require(predicted.input.btSample.seq == 1,
            "predicted frame should retain the previous BT sequence");
    Require(predicted.input.btSample.pressure == 220,
            "predicted frame should retain the previous BT pressure");
#if EGOTOUCH_DIAG
    Require(!predicted.debug.coord.pressureIsReal,
            "predicted frame should mark pressure as synthetic in diagnostics");
    Require(predicted.debug.coord.btSeq == 1,
            "predicted frame should keep the previous BT sequence in diagnostics");
    Require(predicted.debug.coord.predictedAgeFrames >= 1,
            "predicted frame should increase diagnostic prediction age");
#endif

    const auto releaseFrame = RunFrame(
        pipeline,
        BuildCombinedStylusFrame(0x00FF, 0x00FF, {}, 0, 0, {}),
        220,
        20,
        false);
    const auto release = ContractView(releaseFrame);
    Require(!release.output.valid, "release frame should clear output validity");
    Require(!release.output.tipDown, "release frame should clear tip-down output");
    Require(release.output.pressure == 0, "release frame should close pressure output");

    const auto hoverFrame = RunFrame(pipeline, raw, 220, 24, false);
    const auto hover = ContractView(hoverFrame);
    Require(hover.output.valid, "next TX1 frame should solve output again");
    Require(hover.output.tipDown,
            "TX1 evidence should still reopen tip-down without a fresh BT sample");
    Require(hover.output.pressure == 0,
            "stale BT sample should not reopen pressure output");

    const auto resumedFrame = RunFrame(pipeline, raw, 260, 32, true);
    const auto resumed = ContractView(resumedFrame);
    Require(resumed.output.pressure > 0,
            "fresh BT sample after release should reopen pressure output");
    Require(resumed.input.btSample.seq >= 2,
            "fresh BT sample should advance the visible BT sequence");
#if EGOTOUCH_DIAG
    Require(resumed.debug.coord.pressureIsReal,
            "fresh BT sample should restore real-pressure diagnostics");
    Require(resumed.debug.coord.btSeq >= 2,
            "fresh BT sample should advance the diagnostic BT sequence");
#endif
}

void TestStylusPipelineRealZeroDropsPressureButKeepsTipDownWhenTx1StillDown() {
    StylusPipeline pipeline;
    pipeline.LoadConfig("sp.filterMode", "2");

    const auto raw = BuildCombinedStylusFrame(
        10,
        10,
        MakeCrossGrid(12000, 9000, 7000, 5000, 4, 4),
        10,
        10,
        {});

    const auto downFrame = RunFrame(pipeline, raw, 240, 8, true);
    const auto down = ContractView(downFrame);
    Require(down.output.tipDown, "down frame should project tip-down output");

    const auto zeroFrame = RunFrame(pipeline, raw, 0, 16, true);
    const auto zero = ContractView(zeroFrame);
    Require(zero.output.valid, "real-zero frame should keep the stylus point valid");
    Require(zero.output.pressure == 0,
            "real-zero BT sample should drop pressure immediately");
    Require(zero.output.tipDown,
            "real-zero BT sample should keep tip-down while TX1 still indicates down");
    Require(zero.input.btSample.pressure == 0,
            "real-zero frame should snapshot zero BT pressure");
    Require(zero.input.btSample.seq >= 2,
            "real-zero frame should advance BT sequence");
#if EGOTOUCH_DIAG
    Require(zero.debug.coord.pressureIsReal,
            "real-zero frame should still be marked as real in diagnostics");
#endif
}

void TestStylusPipelineInvalidZeroStateClearsOutput() {
    StylusPipeline pipeline;
    pipeline.LoadConfig("sp.filterMode", "2");

    const auto frame = RunFrame(
        pipeline,
        BuildCombinedStylusFrame(0x00FF, 0x00FF, {}, 0, 0, {}),
        100,
        8,
        true);
    const auto stylus = ContractView(frame);

    Require(stylus.input.slaveValid,
            "full-length no-signal frame should still report slave-valid input");
    Require(!stylus.output.valid,
            "invalid-zero frame should clear output validity");
    Require(!stylus.output.inRange,
            "invalid-zero frame should clear in-range output");
    Require(!stylus.output.tipDown,
            "invalid-zero frame should clear tip-down output");
    Require(stylus.output.pressure == 0,
            "invalid-zero frame should clear output pressure");
}

void TestStylusPipelineShortFrameClearsPreviousOutput() {
    StylusPipeline pipeline;

    const auto validRaw = BuildCombinedStylusFrame(
        10,
        10,
        MakeCrossGrid(12000, 9000, 7000, 5000, 4, 4),
        10,
        10,
        {});
    const auto validFrame = RunFrame(pipeline, validRaw, 240, 8, true);
    const auto valid = ContractView(validFrame);
    Require(valid.output.valid, "precondition should project valid output");
    Require(valid.output.pressure > 0, "precondition should project pressure");
    Require(valid.output.tipDown, "precondition should project tip-down output");

    const std::vector<uint8_t> shortRaw(10, 0);
    const auto shortFrame = RunFrame(pipeline, shortRaw, 240, 16, false);
    const auto stylus = ContractView(shortFrame);

    Require(!stylus.input.slaveValid,
            "short frame should clear slave-valid input");
    Require(!stylus.output.valid,
            "short frame should clear output validity");
    Require(stylus.output.pressure == 0,
            "short frame should clear output pressure");
    Require(stylus.output.point.pressure == 0,
            "short frame should clear output point pressure");
    Require(!stylus.output.tipDown,
            "short frame should clear tip-down output");
}

void TestStylusPipelineInvalidPathProjectsFailureStage() {
    StylusPipeline pipeline;

    std::array<uint16_t, kGridDim * kGridDim> flatGrid{};
    flatGrid.fill(100);
    const auto rawNoPeak = BuildCombinedStylusFrame(10, 10, flatGrid, 10, 10, {});

    const auto frame = RunFrame(pipeline, rawNoPeak, 500, 8, true);
    const auto stylus = ContractView(frame);
    Require(!stylus.output.valid, "no-peak frame should not project valid output");
    Require(stylus.output.pipelineStage == 3,
            "contract output should expose peak-detection failure stage");
}

void TestStylusPipelineConfigRoundTrip() {
    StylusPipeline pipeline;
    pipeline.LoadConfig("sp.enableSlaveChecksum", "1");
    pipeline.LoadConfig("sp.emitPacketWhenInvalid", "0");
    pipeline.LoadConfig("sp.recheckEnabled", "0");
    pipeline.LoadConfig("sp.recheckThBase", "888");
    pipeline.LoadConfig("sp.recheckThMulti", "1333");
    pipeline.LoadConfig("sp.tx1InkEnterTh", "9000");
    pipeline.LoadConfig("sp.tx1LiftSuspiciousTh", "7000");
    pipeline.LoadConfig("sp.tx1LiftAbsoluteTh", "4500");
    pipeline.LoadConfig("sp.cmfEnabled", "0");
    pipeline.LoadConfig("sp.cmfWindowSize", "5");
    pipeline.LoadConfig("sp.pressPolyEnabled", "0");
    pipeline.LoadConfig("sp.pressGain", "177");
    pipeline.LoadConfig("sp.filterMode", "2");

    pipeline.LoadConfig("sp.noPressEnabled", "1");
    pipeline.LoadConfig("sp.noPressBaseTh", "9999");
    pipeline.LoadConfig("sp.noPressSyntheticMin", "12");
    pipeline.LoadConfig("sp.sigSuppressEnabled", "1");
    pipeline.LoadConfig("sp.edgeSigSuppressEnter", "1444");
    pipeline.LoadConfig("sp.btMapMode", "2");
    pipeline.LoadConfig("sp.pressIirQ8", "77");

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    Require(saved.find("sp.enableSlaveChecksum=1") != std::string::npos,
            "saved config should include checksum toggle");
    Require(saved.find("sp.emitPacketWhenInvalid=0") != std::string::npos,
            "saved config should include VHF packet emission toggle");
    Require(saved.find("sp.recheckEnabled=0") != std::string::npos,
            "saved config should include recheck toggle");
    Require(saved.find("sp.recheckThBase=888") != std::string::npos,
            "saved config should include recheck base threshold");
    Require(saved.find("sp.recheckThMulti=1333") != std::string::npos,
            "saved config should include recheck multi threshold");
    Require(saved.find("sp.tx1InkEnterTh=9000") != std::string::npos,
            "saved config should include TX1 enter threshold");
    Require(saved.find("sp.pressGain=177") != std::string::npos,
            "saved config should include active pressure gain");
    Require(saved.find("sp.noPressBaseTh=") == std::string::npos,
            "saved config should not emit deprecated no-press keys");
    Require(saved.find("sp.sigSuppressEnabled=") == std::string::npos,
            "saved config should not emit deprecated suppress keys");
    Require(saved.find("sp.edgeSigSuppressEnter=") == std::string::npos,
            "saved config should not emit deprecated edge-suppress keys");
    Require(saved.find("sp.btMapMode=") == std::string::npos,
            "saved config should not emit deprecated BT-map mode");
    Require(saved.find("sp.pressIirQ8=") == std::string::npos,
            "saved config should not emit deprecated pressure IIR key");

    StylusPipeline loaded;
    LoadFromSavedText(loaded, saved);

    std::ostringstream outLoaded;
    loaded.SaveConfig(outLoaded);
    const std::string savedLoaded = outLoaded.str();
    Require(savedLoaded.find("sp.emitPacketWhenInvalid=0") != std::string::npos,
            "loaded config should preserve VHF packet emission toggle");
    Require(savedLoaded.find("sp.recheckEnabled=0") != std::string::npos,
            "loaded config should preserve recheck toggle");
    Require(savedLoaded.find("sp.pressGain=177") != std::string::npos,
            "loaded config should preserve active pressure gain");
    Require(savedLoaded.find("sp.noPressSyntheticMin=") == std::string::npos,
            "loaded config should not re-emit deprecated no-press keys");
    Require(savedLoaded.find("sp.sigSuppressEnabled=") == std::string::npos,
            "loaded config should not re-emit deprecated suppress keys");
    Require(savedLoaded.find("sp.edgeSigSuppressEnter=") == std::string::npos,
            "loaded config should not re-emit deprecated edge-suppress keys");
    Require(savedLoaded.find("sp.btMapMode=") == std::string::npos,
            "loaded config should not re-emit deprecated BT-map mode");
    Require(savedLoaded.find("sp.pressIirQ8=") == std::string::npos,
            "loaded config should not re-emit deprecated pressure IIR key");

    const auto schema = loaded.GetConfigSchema();
    auto hasKey = [&](const char* key) {
        for (const auto& param : schema) {
            if (param.key == key) {
                return true;
            }
        }
        return false;
    };

    Require(hasKey("sp.enableSlaveChecksum"),
            "schema should expose checksum toggle");
    Require(hasKey("sp.emitPacketWhenInvalid"),
            "schema should expose VHF packet emission toggle");
    Require(hasKey("sp.recheckEnabled"),
            "schema should expose recheck toggle");
    Require(hasKey("sp.recheckThMulti"),
            "schema should expose recheck multi threshold");
    Require(hasKey("sp.pressGain"),
            "schema should expose active pressure gain");
    Require(!hasKey("sp.noPressEnabled"),
            "schema should not expose deprecated no-press keys");
    Require(!hasKey("sp.sigSuppressEnabled"),
            "schema should not expose deprecated suppress keys");
    Require(!hasKey("sp.edgeSigSuppressEnter"),
            "schema should not expose deprecated edge-suppress keys");
    Require(!hasKey("sp.btMapMode"),
            "schema should not expose deprecated BT-map mode");
    Require(!hasKey("sp.pressIirQ8"),
            "schema should not expose deprecated pressure IIR key");
}

void TestGridPeakDetectorNoPeak() {
    GridPeakDetector detector;
    int16_t grid[kGridDim][kGridDim]{};

    const auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(!analysis.peak.valid, "empty grid should not produce a peak");
    Require(analysis.projection.peakIdxDim1 == -1,
            "empty grid should not produce a dim1 projection peak");
    Require(analysis.projection.peakIdxDim2 == -1,
            "empty grid should not produce a dim2 projection peak");
}

void TestGridPeakDetectorCenterPeakAndProjection() {
    GridPeakDetector detector;
    int16_t grid[kGridDim][kGridDim]{};
    const auto src = MakeCrossGrid(16000, 14000, 12000, 10000);
    for (int r = 0; r < kGridDim; ++r) {
        for (int c = 0; c < kGridDim; ++c) {
            grid[r][c] = static_cast<int16_t>(src[static_cast<size_t>(r * kGridDim + c)]);
        }
    }

    const auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(analysis.peak.valid, "center cross should produce a peak");
    Require(analysis.peak.peakRow == 4 && analysis.peak.peakCol == 4,
            "center cross should peak at the center cell");
    Require(analysis.projection.peakIdxDim1 == 4 && analysis.projection.peakIdxDim2 == 4,
            "center cross projection should peak on the center axis");
    Require(analysis.projection.spanDim1 == 3 && analysis.projection.spanDim2 == 3,
            "default projection radius should span three rows and columns");
}

void TestGridPeakDetectorEdgePeak() {
    GridPeakDetector detector;
    int16_t grid[kGridDim][kGridDim]{};
    const auto src = MakeCrossGrid(14000, 9000, 6000, 4000, 0, 0);
    for (int r = 0; r < kGridDim; ++r) {
        for (int c = 0; c < kGridDim; ++c) {
            grid[r][c] = static_cast<int16_t>(src[static_cast<size_t>(r * kGridDim + c)]);
        }
    }

    const auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(analysis.peak.valid, "corner cross should produce a peak");
    Require(analysis.peak.peakRow == 0 && analysis.peak.peakCol == 0,
            "corner cross should peak at the corner cell");
    Require(analysis.projection.peakIdxDim1 == 0 && analysis.projection.peakIdxDim2 == 0,
            "corner cross projection should clamp to the corner index");
}

void TestGridPeakDetectorConnectedReject() {
    GridPeakDetector detector;
    detector.maxConnected = 5;
    int16_t grid[kGridDim][kGridDim]{};
    grid[4][4] = 300;
    grid[3][4] = 250;
    grid[5][4] = 250;
    grid[4][3] = 250;
    grid[4][5] = 250;

    const auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(!analysis.peak.valid,
            "connected region at or above maxConnected should be rejected");
}

void TestGridPeakDetectorNeighborSumAndTieBreak() {
    GridPeakDetector detector;
    int16_t grid[kGridDim][kGridDim]{};

    const auto peakA = MakeCrossGrid(12000, 10000, 8000, 6000, 2, 2);
    const auto peakB = MakeCrossGrid(14000, 12000, 9000, 7000, 6, 6);
    for (size_t i = 0; i < peakA.size(); ++i) {
        grid[i / kGridDim][i % kGridDim] =
            static_cast<int16_t>(std::max<uint16_t>(peakA[i], peakB[i]));
    }

    auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(analysis.peak.valid, "dual isolated peaks should still yield a valid peak");
    Require(analysis.peak.peakRow == 6 && analysis.peak.peakCol == 6,
            "larger 3x3 neighbor sum should win peak selection");

    int16_t tieGrid[kGridDim][kGridDim]{};
    const auto tieA = MakeCrossGrid(11000, 9000, 7000, 5000, 2, 2);
    const auto tieB = MakeCrossGrid(11000, 9000, 7000, 5000, 6, 6);
    for (size_t i = 0; i < tieA.size(); ++i) {
        tieGrid[i / kGridDim][i % kGridDim] =
            static_cast<int16_t>(std::max<uint16_t>(tieA[i], tieB[i]));
    }

    analysis = detector.AnalyzePeakAndProjection(tieGrid);
    Require(analysis.peak.peakRow == 2 && analysis.peak.peakCol == 2,
            "equal neighbor sums should preserve row-major tie breaking");
}

void TestGridPeakDetectorProjectionRadius() {
    GridPeakDetector detector;
    detector.projRadius = 1;
    int16_t grid[kGridDim][kGridDim]{};
    const auto src = MakeCrossGrid(16000, 14000, 12000, 10000);
    for (int r = 0; r < kGridDim; ++r) {
        for (int c = 0; c < kGridDim; ++c) {
            grid[r][c] = static_cast<int16_t>(src[static_cast<size_t>(r * kGridDim + c)]);
        }
    }

    const auto analysis = detector.AnalyzePeakAndProjection(grid);
    Require(analysis.projection.spanDim1 == 3 && analysis.projection.spanDim2 == 3,
            "custom projection radius should change projection span");
}

void TestStylusPipelineLegacyAliasRoundTripKeepsCanonicalKey() {
    StylusPipeline pipeline;
    pipeline.LoadConfig("sp.coordEdgeCompBit3", "1");

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    Require(saved.find("sp.triEdgeSecondaryBlend=1") != std::string::npos,
            "legacy alias should persist through canonical triEdgeSecondaryBlend key");
    Require(saved.find("sp.coordEdgeCompBit3=") == std::string::npos,
            "legacy alias key should not be emitted in saved config");

    const auto schema = pipeline.GetConfigSchema();
    bool foundCanonical = false;
    bool foundAlias = false;
    for (const auto& param : schema) {
        if (param.key == "sp.triEdgeSecondaryBlend") {
            foundCanonical = true;
        }
        if (param.key == "sp.coordEdgeCompBit3") {
            foundAlias = true;
        }
    }
    Require(foundCanonical, "schema should expose canonical secondary blend key");
    Require(!foundAlias, "schema should not expose legacy alias key");
}

void TestStylusPipelineProcessReturnsTrueWithoutSolvedOutput() {
    StylusPipeline pipeline;
    const auto raw = BuildCombinedStylusFrame(0x00FF, 0x00FF, {}, 0, 0, {});

    HeatmapFrame frame{};
    frame.rawPtr = raw.data();
    frame.rawLen = raw.size();

    const bool processed = pipeline.Process(frame);
    const auto stylus = ContractView(frame);

    Require(processed,
            "Process should still report success even without a solved stylus output");
    Require(!stylus.output.valid,
            "Process success should not imply a valid stylus output");
}

} // namespace

int main() {
    try {
        TestPressureStageSignalSuppressHysteresis();
        TestPressureStageEdgeSuppressHysteresis();
        TestPressureStagePredictsBetweenRealSamples();
        TestPressureStageRealZeroDropsImmediately();
        TestBtPressBufferDefaultMode();
        TestBtPressBufferOnCellMapping();
        TestBtPressBufferInCellMapping();
        TestBtPressBufferReset();
        TestBtPressBufferSeqSnapshot();
        TestStylusPipelineValidFrameProjectsInputOutputAndInteropContract();
        TestStylusPipelineWeakSignalKeepsSolvedPointButStaysOutOfTipDown();
        TestStylusPipelinePredictedPressureProjectsBtInputContract();
        TestStylusPipelineRealZeroDropsPressureButKeepsTipDownWhenTx1StillDown();
        TestStylusPipelineInvalidZeroStateClearsOutput();
        TestStylusPipelineShortFrameClearsPreviousOutput();
        TestStylusPipelineInvalidPathProjectsFailureStage();
        TestStylusPipelineConfigRoundTrip();
        TestGridPeakDetectorNoPeak();
        TestGridPeakDetectorCenterPeakAndProjection();
        TestGridPeakDetectorEdgePeak();
        TestGridPeakDetectorConnectedReject();
        TestGridPeakDetectorNeighborSumAndTieBreak();
        TestGridPeakDetectorProjectionRadius();
        TestStylusPipelineLegacyAliasRoundTripKeepsCanonicalKey();
        TestStylusPipelineProcessReturnsTrueWithoutSolvedOutput();
        std::cout << "[TEST] Stylus fast ink tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
