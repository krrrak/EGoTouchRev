#include "StylusSolver/CoordinateSolver.hpp"
#include "StylusSolver/GridFeatureExtractor.hpp"
#include "StylusSolver/LinearFilterProcess.hpp"
#include "StylusSolver/StylusPipeline.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Asa::AsaCoorResult Coor(int32_t dim1, int32_t dim2) {
    Asa::AsaCoorResult coor{};
    coor.valid = true;
    coor.dim1 = dim1;
    coor.dim2 = dim2;
    return coor;
}

void ConfigureFastTestFilter(Solvers::Stylus::LinearFilterProcess& filter) {
    filter.m_sparseMoveThreshold = 10;
    filter.m_shortMoveThreshold = 4;
    filter.m_minFitPoints = 4;
    filter.m_anchorMoveThreshold = 1;
    filter.m_enterCountMax = 2;
    filter.m_exitCountMax = 2;
    filter.m_dragLimit = 32;
    filter.m_enterMaxDistSq = 4;
    filter.m_exitDistSq = 10000;
}

Asa::AsaCoorResult Feed(Solvers::Stylus::LinearFilterProcess& filter,
                        int32_t dim1,
                        int32_t dim2,
                        bool pressure = true) {
    return filter.Process(Coor(dim1, dim2), pressure, 60000, 40000);
}

void FeedStraightToStable(Solvers::Stylus::LinearFilterProcess& filter) {
    ConfigureFastTestFilter(filter);
    for (int i = 0; i < 9; ++i) {
        Feed(filter, 1000 + i * 80, 2000);
    }
    Require(filter.State() == 5,
            "straight sparse points should advance the filter into straight-line state");
}

void LoadFromSavedText(Solvers::StylusPipeline& pipeline, const std::string& saved) {
    std::istringstream in(saved);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        pipeline.LoadConfig(line.substr(0, eq), line.substr(eq + 1));
    }
}

void SetProjectionPeak(Asa::AsaProjection& projection,
                       int peakDim1,
                       int peakDim2,
                       int leftDim1,
                       int centerDim1,
                       int rightDim1,
                       int leftDim2,
                       int centerDim2,
                       int rightDim2) {
    projection.Clear();
    projection.peakIdxDim1 = peakDim1;
    projection.peakIdxDim2 = peakDim2;
    projection.dim1[peakDim1 - 1] = leftDim1;
    projection.dim1[peakDim1] = centerDim1;
    projection.dim1[peakDim1 + 1] = rightDim1;
    projection.dim2[peakDim2 - 1] = leftDim2;
    projection.dim2[peakDim2] = centerDim2;
    projection.dim2[peakDim2 + 1] = rightDim2;
}

void TestDefaultParametersMatchAsaTable() {
    Solvers::Stylus::LinearFilterProcess filter;

    Require(filter.m_sparseMoveThreshold == 64,
            "default straight sparse threshold should match TSACore");
    Require(filter.m_shortMoveThreshold == 16,
            "default short-distance threshold should match TSACore");
    Require(filter.m_anchorMoveThreshold == 32,
            "default enter/exit anchor threshold should match TSACore");
    Require(filter.m_dragLimit == 0x20,
            "default drag limit should match ASA table +0xa62");
    Require(filter.m_enterMaxDistSq == 0x0384,
            "default enter max distance should match ASA table +0xa64");
    Require(filter.m_exitDistSq == 0x0e10,
            "default exit distance should match ASA table +0xa66");
}

void TestPressureResetReturnsRawAndStateZero() {
    Solvers::Stylus::LinearFilterProcess filter;
    FeedStraightToStable(filter);

    const auto out = Feed(filter, 2500, 2300, false);
    Require(filter.State() == 0,
            "zero pressure should reset linear filter state");
    Require(out.dim1 == 2500 && out.dim2 == 2300,
            "zero pressure should pass raw coordinate through");
}

void TestStraightLineEntryAndDragLimit() {
    Solvers::Stylus::LinearFilterProcess filter;
    FeedStraightToStable(filter);

    const auto raw = Coor(1800, 2100);
    const auto out = filter.Process(raw, true, 60000, 40000);
    const int rawDist = std::abs(raw.dim2 - 2000);
    const int outDist = std::abs(out.dim2 - 2000);

    Require(outDist < rawDist,
            "straight-line mode should drag the point toward the fitted line");
    Require(std::abs(out.dim2 - raw.dim2) <= filter.m_dragLimit,
            "straight-line drag should be capped by drag limit");
}

void TestLargeDeviationExitsStraightLine() {
    Solvers::Stylus::LinearFilterProcess filter;
    FeedStraightToStable(filter);
    filter.m_exitDistSq = 100;

    Feed(filter, 1800, 2300);
    Require(filter.State() == 6,
            "large point-to-line distance should enter exit state");
}

void TestDirectionChangeExitsStraightLine() {
    Solvers::Stylus::LinearFilterProcess filter;
    FeedStraightToStable(filter);
    filter.m_exitDistSq = 1000000;

    Feed(filter, 1200, 2000);
    Require(filter.State() == 6,
            "sharp direction change should enter exit state");
}

void TestClampToSensorBounds() {
    Solvers::Stylus::LinearFilterProcess filter;
    ConfigureFastTestFilter(filter);

    const auto out = filter.Process(Coor(5000, -100), true, 3000, 2000);
    Require(out.dim1 == 3000,
            "filtered output should clamp dim1 to sensor maximum");
    Require(out.dim2 == 0,
            "filtered output should clamp dim2 to zero");
}

void TestBypassModeResetsPostFilter() {
    Solvers::HeatmapFrame frame{};
    auto& stylus = frame.stylus;
    stylus.runtime.tx1.coordinate.reportGlobalCoor = Coor(1234, 2345);
    stylus.runtime.decision.inRangeCandidate = true;
    stylus.runtime.decision.tipDownCandidate = true;
    stylus.runtime.pressure.outputPressure = 300;

    Solvers::Stylus::StylusPostProcessor post;
    post.m_filterMode = Solvers::Stylus::StylusPostProcessor::Bypass;
    post.m_linearFilter.m_sparseMoveThreshold = 1;
    post.Process(frame);

    Require(stylus.runtime.post.finalCoor.dim1 == 1234 &&
            stylus.runtime.post.finalCoor.dim2 == 2345,
            "bypass mode should copy raw coordinate");
    Require(stylus.runtime.post.linearFilterState == 0,
            "bypass mode should reset linear filter state");
}

void TestConfigRoundTrip() {
    Solvers::StylusPipeline pipeline;
    pipeline.m_post.m_linearFilter.m_enabled = false;
    pipeline.m_post.m_linearFilter.m_dragLimit = 77;
    pipeline.m_post.m_linearFilter.m_enterMaxDistSq = 88;
    pipeline.m_post.m_linearFilter.m_exitDistSq = 99;
    pipeline.m_post.m_linearFilter.m_sparseMoveThreshold = 11;
    pipeline.m_post.m_linearFilter.m_shortMoveThreshold = 12;
    pipeline.m_post.m_linearFilter.m_anchorMoveThreshold = 13;
    pipeline.m_post.m_linearFilter.m_minFitPoints = 14;

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    Require(saved.find("sp.linearFilterEnabled=0") != std::string::npos,
            "saved config should include linear filter enabled flag");
    Require(saved.find("sp.linearFilterDragLimit=77") != std::string::npos,
            "saved config should include linear filter drag limit");
    Require(saved.find("sp.linearFilterMinFitPoints=14") != std::string::npos,
            "saved config should include linear filter fit point count");

    Solvers::StylusPipeline loaded;
    LoadFromSavedText(loaded, saved);

    Require(!loaded.m_post.m_linearFilter.m_enabled,
            "linear filter enabled flag should round-trip");
    Require(loaded.m_post.m_linearFilter.m_dragLimit == 77,
            "linear filter drag limit should round-trip");
    Require(loaded.m_post.m_linearFilter.m_enterMaxDistSq == 88,
            "linear filter enter threshold should round-trip");
    Require(loaded.m_post.m_linearFilter.m_exitDistSq == 99,
            "linear filter exit threshold should round-trip");
    Require(loaded.m_post.m_linearFilter.m_sparseMoveThreshold == 11,
            "linear filter sparse threshold should round-trip");
    Require(loaded.m_post.m_linearFilter.m_shortMoveThreshold == 12,
            "linear filter short threshold should round-trip");
    Require(loaded.m_post.m_linearFilter.m_anchorMoveThreshold == 13,
            "linear filter anchor threshold should round-trip");
    Require(loaded.m_post.m_linearFilter.m_minFitPoints == 14,
            "linear filter min fit points should round-trip");
}

void TestGridFeatureExtractorAlignsTx2WithFactoryFlow() {
    Solvers::HeatmapFrame frame{};
    auto& stylus = frame.stylus;
    stylus.runtime.parse.valid = true;
    stylus.runtime.rawGrid.asaGrid.tx1.valid = true;
    stylus.runtime.rawGrid.asaGrid.tx2.valid = true;
    stylus.runtime.rawGrid.asaGrid.tx1.anchorRow = 4;
    stylus.runtime.rawGrid.asaGrid.tx1.anchorCol = 4;
    stylus.runtime.rawGrid.asaGrid.tx2.anchorRow = 4;
    stylus.runtime.rawGrid.asaGrid.tx2.anchorCol = 4;
    stylus.runtime.rawGrid.asaGrid.tx1.grid[4][4] = 200;
    stylus.runtime.rawGrid.asaGrid.tx2.grid[4][4] = 130;
    stylus.runtime.rawGrid.asaGrid.tx2.grid[5][4] = 150;

    Solvers::Stylus::GridFeatureExtractor extractor;
    extractor.Process(frame);

    const auto& tx2 = stylus.runtime.tx2.feature;
    Require(tx2.grid[4][4] == 90,
            "TX2 grid should subtract TX1 contribution before peak detection");
    Require(tx2.peakTable.count == 1,
            "TX2 should retain one peak region in this fixture");
    Require(tx2.peakTable.strongestSlot == 0 && tx2.peakTable.weakestSlot == 0,
            "single TX2 region should be both strongest and weakest");
    Require(tx2.peak.valid && tx2.peak.peakRow == 5 && tx2.peak.peakCol == 4,
            "TX2 strongest peak should resolve to the higher local maximum");
    Require(tx2.refinedLocalCoor.valid,
            "TX2 refined local coordinate should be populated");
    Require(tx2.refinedLocalCoor.dim1 == 4 * Asa::kCoorUnit + (Asa::kCoorUnit / 2) &&
            tx2.refinedLocalCoor.dim2 == 5258,
            "TX2 refined coordinate should follow factory weighted refinement");
    Require(tx2.peakSignal == 240,
            "TX2 peak signal should use the strongest 3x3 sum");
}

void TestTx2SeedThresholdIsGreaterThanNinetyNine() {
    Solvers::HeatmapFrame lowFrame{};
    auto& low = lowFrame.stylus;
    low.runtime.parse.valid = true;
    low.runtime.rawGrid.asaGrid.tx1.valid = true;
    low.runtime.rawGrid.asaGrid.tx2.valid = true;
    low.runtime.rawGrid.asaGrid.tx1.anchorRow = 0;
    low.runtime.rawGrid.asaGrid.tx1.anchorCol = 0;
    low.runtime.rawGrid.asaGrid.tx2.anchorRow = 4;
    low.runtime.rawGrid.asaGrid.tx2.anchorCol = 4;
    low.runtime.rawGrid.asaGrid.tx1.grid[0][0] = 200;
    low.runtime.rawGrid.asaGrid.tx2.grid[4][4] = 99;

    Solvers::Stylus::GridFeatureExtractor extractor;
    extractor.Process(lowFrame);
    Require(low.runtime.tx2.feature.peakTable.count == 0,
            "TX2 candidate value 99 should not seed a peak");

    Solvers::HeatmapFrame highFrame{};
    auto& high = highFrame.stylus;
    high.runtime.parse.valid = true;
    high.runtime.rawGrid.asaGrid.tx1.valid = true;
    high.runtime.rawGrid.asaGrid.tx2.valid = true;
    high.runtime.rawGrid.asaGrid.tx1.anchorRow = 0;
    high.runtime.rawGrid.asaGrid.tx1.anchorCol = 0;
    high.runtime.rawGrid.asaGrid.tx2.anchorRow = 4;
    high.runtime.rawGrid.asaGrid.tx2.anchorCol = 4;
    high.runtime.rawGrid.asaGrid.tx1.grid[0][0] = 200;
    high.runtime.rawGrid.asaGrid.tx2.grid[4][4] = 100;

    extractor.Process(highFrame);
    Require(high.runtime.tx2.feature.peakTable.count == 1,
            "TX2 candidate value 100 should seed a peak under the factory >99 gate");
}

void TestCoordinateSolverUsesTx2RefinedCoordinate() {
    Solvers::HeatmapFrame frame{};
    auto& stylus = frame.stylus;
    stylus.runtime.rawGrid.asaGrid.tx2.valid = true;
    stylus.runtime.tx1.feature.peak.valid = true;
    SetProjectionPeak(stylus.runtime.tx1.feature.projection,
                      4, 4,
                      100, 300, 100,
                      120, 320, 120);
    stylus.runtime.rawGrid.asaGrid.tx1.anchorRow = 4;
    stylus.runtime.rawGrid.asaGrid.tx1.anchorCol = 4;
    stylus.runtime.rawGrid.asaGrid.tx2.anchorRow = 4;
    stylus.runtime.rawGrid.asaGrid.tx2.anchorCol = 4;
    stylus.runtime.tx2.feature.refinedLocalCoor = {1234, 2345, true};
    stylus.runtime.tx2.feature.peak.valid = false;
    stylus.runtime.tx2.feature.projection.peakIdxDim1 = 1;
    stylus.runtime.tx2.feature.projection.peakIdxDim2 = 1;
    stylus.runtime.tx2.feature.projection.dim1[0] = 10;
    stylus.runtime.tx2.feature.projection.dim1[1] = 20;
    stylus.runtime.tx2.feature.projection.dim1[2] = 10;
    stylus.runtime.tx2.feature.projection.dim2[0] = 10;
    stylus.runtime.tx2.feature.projection.dim2[1] = 20;
    stylus.runtime.tx2.feature.projection.dim2[2] = 10;
    stylus.runtime.signal.signalX = 111;
    stylus.runtime.tx2.feature.peakSignal = 777;

    Solvers::Stylus::CoordinateSolver solver;
    solver.Process(frame);

    Require(stylus.runtime.tx2.coordinate.localGridCoor.valid,
            "coordinate solver should accept TX2 refined coordinate");
    Require(stylus.runtime.tx2.coordinate.localGridCoor.dim1 == 1234 &&
            stylus.runtime.tx2.coordinate.localGridCoor.dim2 == 2345,
            "coordinate solver should consume TX2 refined coordinate directly");
    Require(stylus.runtime.signal.signalY == 777,
            "coordinate solver should keep TX2 peak signal in the runtime signal block");
}

} // namespace

int main() {
    try {
        TestDefaultParametersMatchAsaTable();
        TestPressureResetReturnsRawAndStateZero();
        TestStraightLineEntryAndDragLimit();
        TestLargeDeviationExitsStraightLine();
        TestDirectionChangeExitsStraightLine();
        TestClampToSensorBounds();
        TestBypassModeResetsPostFilter();
        TestConfigRoundTrip();
        TestGridFeatureExtractorAlignsTx2WithFactoryFlow();
        TestTx2SeedThresholdIsGreaterThanNinetyNine();
        TestCoordinateSolverUsesTx2RefinedCoordinate();
        std::cout << "[TEST] Stylus linear filter process tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
