#include "TouchSolver/BaselineSubtraction.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

constexpr uint16_t kRawBaseline = 1000;
constexpr uint16_t kRawHighCell = 1400;
constexpr int kPeakRow = 20;
constexpr int kPeakCol = 20;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int16_t PeakValue(const Solvers::HeatmapFrame& frame) {
    return frame.heatmapMatrix[kPeakRow][kPeakCol];
}

int16_t BackgroundValue(const Solvers::HeatmapFrame& frame) {
    return frame.heatmapMatrix[0][0];
}

Solvers::Touch::BaselineInputState BaselineInput(Solvers::Touch::FingerState fingerState) {
    return {true, fingerState};
}

Solvers::Touch::BaselineInputState InvalidMasterInput() {
    return {false, Solvers::Touch::FingerState::Unknown};
}

void FillRaw(Solvers::HeatmapFrame& frame, uint16_t value) {
    for (auto& row : frame.heatmapMatrix) {
        for (auto& cell : row) {
            cell = static_cast<int16_t>(value);
        }
    }
}

void FillRawWithHighCell(Solvers::HeatmapFrame& frame) {
    FillRaw(frame, kRawBaseline);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawHighCell);
}

void PrimeBaseline(Solvers::Touch::BaselineSubtraction& baseline) {
    baseline.m_baseline = kRawBaseline;
    Solvers::HeatmapFrame frame;
    FillRaw(frame, kRawBaseline);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0, "baseline prime frame should subtract to zero");
}

void TestLocalPositivePeakFreezesWithoutReset() {
    Solvers::Touch::BaselineSubtraction baseline;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    for (int i = 0; i < 4; ++i) {
        FillRawWithHighCell(frame);
        baseline.Process(frame);
    }

    Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
            "local positive peak should remain frozen without reset");
}

void TestRequestReacquireFramesOnlyResetsBaseline() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_baseline = kRawBaseline;
    PrimeBaseline(baseline);

    baseline.m_baseline = 2000;
    baseline.RequestReacquireFrames(8);

    Solvers::HeatmapFrame frame;
    FillRaw(frame, 2500);
    baseline.Process(frame);

    Require(PeakValue(frame) == 0,
            "request reacquire should suppress fallback common-mode diff after default reset");
}

void TestFallbackCommonModeOffsetDoesNotRaiseBackground() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_freezeCandidateThreshold = 350;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    FillRaw(frame, kRawBaseline + 600);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawBaseline + 1000);
    baseline.Process(frame);

    Require(BackgroundValue(frame) == 0,
            "fallback common-mode offset should not produce high background output");
    Require(PeakValue(frame) >= baseline.m_freezeCandidateThreshold,
            "fallback common-mode correction should preserve local peak diff");
}

void TestRequestReacquireFramesDoesNotSuppressTouch() {
    Solvers::Touch::BaselineSubtraction baseline;
    PrimeBaseline(baseline);

    baseline.RequestReacquireFrames(8);

    Solvers::HeatmapFrame frame;
    FillRawWithHighCell(frame);
    baseline.Process(frame);

    Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
            "request reacquire should not keep a multi-frame suppression window");
}

void TestResetDropsPreviousDynamicBaselineAndUsesDefault() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_baseline = kRawBaseline;
    PrimeBaseline(baseline);

    baseline.m_baseline = 2000;
    baseline.Reset();

    Solvers::HeatmapFrame frame;
    FillRaw(frame, 2500);
    baseline.Process(frame);

    Require(PeakValue(frame) == 0,
            "reset should initialize from BaselineValue while suppressing fallback common-mode diff");
}

void TestNoFingerUpdatesAllCellsEvenWhenPeakIsHigh() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_noFingerAlphaShift = 0;
    baseline.m_noFingerMaxStep = 2000;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    FillRawWithHighCell(frame);
    baseline.Process(frame, BaselineInput(Solvers::Touch::FingerState::NoFinger));
    Require(PeakValue(frame) == 0,
            "no-finger baseline should suppress output while absorbing every cell");

    FillRawWithHighCell(frame);
    baseline.Process(frame, BaselineInput(Solvers::Touch::FingerState::Finger));
    Require(PeakValue(frame) == 0,
            "high cell seen during confirmed no-finger should be part of baseline later");
}

void TestFingerFreezesCandidatePeakButTracksBackground() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_freezeCandidateThreshold = 350;
    baseline.m_fingerBackgroundAlphaShift = 0;
    baseline.m_fingerBackgroundMaxStep = 2000;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    FillRaw(frame, kRawBaseline + 600);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawBaseline + 1000);

    baseline.Process(frame, BaselineInput(Solvers::Touch::FingerState::Finger));

    Require(BackgroundValue(frame) == 0,
            "finger background cells should continue dynamic baseline tracking");
    Require(PeakValue(frame) >= baseline.m_freezeCandidateThreshold,
            "finger candidate peak cell should be frozen and reported as diff");
}

void TestInvalidMasterDoesNotPolluteBaseline() {
    Solvers::Touch::BaselineSubtraction baseline;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    FillRawWithHighCell(frame);
    baseline.Process(frame, InvalidMasterInput());
    Require(PeakValue(frame) == 0,
            "invalid master frame should produce safe zero output");

    FillRawWithHighCell(frame);
    baseline.Process(frame, BaselineInput(Solvers::Touch::FingerState::Finger));
    Require(PeakValue(frame) >= baseline.m_freezeCandidateThreshold,
            "invalid master frame should not absorb a later candidate touch peak");
}

} // namespace

int main() {
    try {
        TestLocalPositivePeakFreezesWithoutReset();
        TestRequestReacquireFramesOnlyResetsBaseline();
        TestFallbackCommonModeOffsetDoesNotRaiseBackground();
        TestRequestReacquireFramesDoesNotSuppressTouch();
        TestResetDropsPreviousDynamicBaselineAndUsesDefault();
        TestNoFingerUpdatesAllCellsEvenWhenPeakIsHigh();
        TestFingerFreezesCandidatePeakButTracksBackground();
        TestInvalidMasterDoesNotPolluteBaseline();
        std::cout << "[TEST] Touch baseline subtraction tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
