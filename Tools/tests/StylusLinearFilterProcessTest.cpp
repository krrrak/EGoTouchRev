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
    stylus.runtime.tx1.globalCoor = Coor(1234, 2345);
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
        std::cout << "[TEST] Stylus linear filter process tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
