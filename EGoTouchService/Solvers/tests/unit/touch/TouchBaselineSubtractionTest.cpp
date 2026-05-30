#include "TouchSolver/BaselineSubtraction.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

constexpr uint16_t kRawBaseline = 1000;
constexpr uint16_t kRawHighCell = 1300;
constexpr int kPeakRow = 20;
constexpr int kPeakCol = 20;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int16_t PeakValue(const Solvers::HeatmapFrame& frame) {
    return frame.heatmapMatrix[kPeakRow][kPeakCol];
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

void TestLocalPositivePeakFreezesWithoutReacquire() {
    Solvers::Touch::BaselineSubtraction baseline;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    for (int i = 0; i < 4; ++i) {
        FillRawWithHighCell(frame);
        baseline.Process(frame);
    }

    Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
            "local positive peak should remain frozen without reacquire");
}

void TestReacquireSnapshotsLocalPeakAndCorrectsOnRelease() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_settleFrames = 0;
    PrimeBaseline(baseline);

    baseline.RequestReacquireFrames(8);

    Solvers::HeatmapFrame frame;
    FillRawWithHighCell(frame);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0,
            "reacquire should snapshot a wake-time local peak into baseline");

    FillRaw(frame, kRawBaseline);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0,
            "release from a snapshotted peak should correct baseline without negative output");

    for (int i = 0; i < 8; ++i) {
        FillRawWithHighCell(frame);
        baseline.Process(frame);
        Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
                "new long press after release should be preserved by touch freeze");
    }
}

void TestCleanReacquireDoesNotAbsorbLaterLongPress() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_settleFrames = 0;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    baseline.RequestReacquireFrames(8);
    FillRaw(frame, kRawBaseline);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0,
            "clean reacquire frame should output zero");

    for (int i = 0; i < 24; ++i) {
        FillRawWithHighCell(frame);
        baseline.Process(frame);
        Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
                "normal long press after clean reacquire should not be absorbed");
    }
}

void TestTouchDuringSettleIsReportedAfterSettle() {
    Solvers::Touch::BaselineSubtraction baseline;
    baseline.m_settleFrames = 2;
    PrimeBaseline(baseline);

    Solvers::HeatmapFrame frame;
    baseline.RequestReacquireFrames(8);
    FillRaw(frame, kRawBaseline);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0,
            "snapshot settle frame should output zero");

    FillRawWithHighCell(frame);
    baseline.Process(frame);
    Require(PeakValue(frame) == 0,
            "touch during settle should be suppressed only while settling");

    FillRawWithHighCell(frame);
    baseline.Process(frame);
    Require(PeakValue(frame) >= baseline.m_touchFreezeThreshold,
            "touch held through settle should be reported after settle ends");
}

} // namespace

int main() {
    try {
        TestLocalPositivePeakFreezesWithoutReacquire();
        TestReacquireSnapshotsLocalPeakAndCorrectsOnRelease();
        TestCleanReacquireDoesNotAbsorbLaterLongPress();
        TestTouchDuringSettleIsReportedAfterSettle();
        std::cout << "[TEST] Touch baseline reacquire tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
