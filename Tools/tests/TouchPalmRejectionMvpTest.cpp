#include "TouchSolver/PalmRejector.hpp"
#include "TouchSolver/PeakEvaluator.hpp"
#include "TouchSolver/TouchPipeline.h"
#include "TouchSolver/ZoneExpander.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RequireNear(float actual, float expected, float epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        throw std::runtime_error(message);
    }
}

Solvers::MacroZone MakeZone(std::vector<int>& pixels, int minR, int maxR, int minC, int maxC) {
    Solvers::MacroZone zone;
    zone.pixels = std::span<const int>(pixels.data(), pixels.size());
    zone.area = static_cast<int>(pixels.size());
    zone.minR = minR;
    zone.maxR = maxR;
    zone.minC = minC;
    zone.maxC = maxC;
    return zone;
}

void LoadFromSavedText(Solvers::TouchPipeline& pipeline, const std::string& saved) {
    std::istringstream in(saved);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        pipeline.LoadConfig(line.substr(0, eq), line.substr(eq + 1));
    }
}

void TestPalmRejectorPreservesMacroZone() {
    Solvers::HeatmapFrame frame;
    std::vector<int> pixels;
    for (int r = 10; r < 18; ++r) {
        for (int c = 20; c < 28; ++c) {
            pixels.push_back(r * 60 + c);
            frame.heatmapMatrix[r][c] = 200;
        }
    }

    auto zone = MakeZone(pixels, 10, 17, 20, 27);
    zone.signalSum = static_cast<int>(pixels.size()) * 200;
    std::vector<Solvers::MacroZone> zones{zone};

    Solvers::Touch::PalmRejector rejector;
    rejector.m_areaThreshold = 10;
    rejector.m_signalSumThreshold = 1000;
    const int rejected = rejector.Process(zones, frame);

    Require(rejected == 0, "PalmRejector should no longer reject zones early");
    Require(zones.size() == 1, "PalmRejector should preserve palm-like MacroZone");
    Require(rejector.GetZoneFeatures().size() == 1, "PalmRejector should record zone features");
    Require(rejector.GetZoneFeatures()[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "large preserved MacroZone should be classified as palm likely");
}

void TestPeakEvaluatorKeepsSharpPeakInPalmCandidate() {
    Solvers::HeatmapFrame frame;
    for (int r = 18; r <= 22; ++r) {
        for (int c = 18; c <= 22; ++c) {
            frame.heatmapMatrix[r][c] = 100;
        }
    }
    frame.heatmapMatrix[20][20] = 600;

    Solvers::Touch::Peak peak;
    peak.r = 20;
    peak.c = 20;
    peak.z = 600;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    Solvers::Touch::MacroZoneFeature zone;
    zone.zoneIndex = 0;
    zone.area = 45;
    zone.palmClass = Solvers::Touch::PalmClass::PalmCandidate;
    zone.palmScore = 0.65f;
    std::vector<Solvers::Touch::MacroZoneFeature> zones{zone};

    Solvers::Touch::PeakEvaluator evaluator;
    evaluator.Process(frame, peaks, zones);
    const auto evals = evaluator.GetEvaluations();

    Require(evals.size() == 1, "PeakEvaluator should emit one evaluation per peak");
    Require(evals[0].palmClass == Solvers::Touch::PalmClass::FingerLikely,
            "sharp peak in palm candidate should be finger likely");
    Require(evals[0].allowContact, "finger-like peak in palm candidate should remain eligible");
}

void TestPeakEvaluatorSuppressesFlatPalmPeak() {
    Solvers::HeatmapFrame frame;
    for (int r = 18; r <= 22; ++r) {
        for (int c = 18; c <= 22; ++c) {
            frame.heatmapMatrix[r][c] = 150;
        }
    }
    frame.heatmapMatrix[20][20] = 160;

    Solvers::Touch::Peak peak;
    peak.r = 20;
    peak.c = 20;
    peak.z = 160;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    Solvers::Touch::MacroZoneFeature zone;
    zone.zoneIndex = 0;
    zone.area = 70;
    zone.palmClass = Solvers::Touch::PalmClass::PalmLikely;
    zone.palmScore = 0.9f;
    std::vector<Solvers::Touch::MacroZoneFeature> zones{zone};

    Solvers::Touch::PeakEvaluator evaluator;
    evaluator.Process(frame, peaks, zones);
    const auto evals = evaluator.GetEvaluations();

    Require(evals[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "flat peak in palm-like zone should be palm likely");
    Require(!evals[0].allowContact, "palm likely peak should not become contact peak");
    Require(evals[0].palmEvidenceOnly, "suppressed palm peak should remain palm evidence");
}

void TestPeakEvaluatorSuppressesBroadPalmPressurePeak() {
    Solvers::HeatmapFrame frame;
    for (int r = 18; r <= 22; ++r) {
        for (int c = 18; c <= 22; ++c) {
            frame.heatmapMatrix[r][c] = 1800;
        }
    }
    frame.heatmapMatrix[20][20] = 3491;

    Solvers::Touch::Peak peak;
    peak.r = 20;
    peak.c = 20;
    peak.z = 3491;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    Solvers::Touch::MacroZoneFeature zone;
    zone.zoneIndex = 0;
    zone.area = 134;
    zone.palmClass = Solvers::Touch::PalmClass::PalmLikely;
    zone.palmScore = 0.75f;
    std::vector<Solvers::Touch::MacroZoneFeature> zones{zone};

    Solvers::Touch::PeakEvaluator evaluator;
    evaluator.Process(frame, peaks, zones);
    const auto evals = evaluator.GetEvaluations();

    Require(evals[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "broad high-signal palm pressure peak should be palm likely");
    Require(!evals[0].allowContact, "broad palm pressure peak should not create a contact");
    Require(evals[0].palmEvidenceOnly, "suppressed broad palm peak should remain palm evidence");
}

void TestZoneExpanderSkipsPalmLikelyPeak() {
    Solvers::HeatmapFrame frame;
    frame.heatmapMatrix[20][20] = 300;

    Solvers::Touch::Peak peak;
    peak.r = 20;
    peak.c = 20;
    peak.z = 300;
    peak.id = 7;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    Solvers::Touch::PeakEvaluation eval;
    eval.palmClass = Solvers::Touch::PalmClass::PalmLikely;
    eval.allowContact = false;
    eval.palmEvidenceOnly = true;
    std::vector<Solvers::Touch::PeakEvaluation> evals{eval};

    Solvers::Touch::ZoneExpander expander;
    expander.m_dilateErode = false;
    expander.Process(frame, peaks, 130, evals);

    Require(frame.contacts.empty(), "palm-only peak should not create a touch contact");
    Require(expander.GetEdgeInfos().empty(), "suppressed palm-only peak should not create contact edge info");
}

void TestPalmConfigRoundTrip() {
    Solvers::TouchPipeline pipeline;
    pipeline.m_palmReject.m_enabled = false;
    pipeline.m_palmReject.m_areaThreshold = 77;
    pipeline.m_palmReject.m_signalSumThreshold = 123456;
    pipeline.m_palmReject.m_densityThresholdLow = 321.5f;
    pipeline.m_palmReject.m_areaMinForDensity = 22;
    pipeline.m_palmReject.m_elongatedEnabled = false;
    pipeline.m_palmReject.m_elongatedMinArea = 12;
    pipeline.m_palmReject.m_elongatedAspectRatio = 3.25f;
    pipeline.m_palmReject.m_analyzerEnabled = false;
    pipeline.m_palmReject.m_candidateAreaThreshold = 44;
    pipeline.m_palmReject.m_candidateSignalThreshold = 65432;
    pipeline.m_palmReject.m_likelyAreaThreshold = 66;
    pipeline.m_palmReject.m_fillRatioThreshold = 0.55f;
    pipeline.m_palmReject.m_flatSharpnessThreshold = 1.22f;
    pipeline.m_palmReject.m_strongPeakProminence = 222;
    pipeline.m_peakEval.m_enabled = false;
    pipeline.m_peakEval.m_fingerProminence = 333;
    pipeline.m_peakEval.m_fingerSharpness = 1.77f;
    pipeline.m_peakEval.m_palmSharpnessMax = 1.11f;
    pipeline.m_peakEval.m_ambiguousMargin = 0.25f;
    pipeline.m_peakEval.m_palmAwareExpansionEnabled = false;
    pipeline.m_peakEval.m_fingerInPalmThresholdRatio = 0.8f;
    pipeline.m_peakEval.m_fingerInPalmMaxRadius = 4;
    pipeline.m_peakEval.m_palmLikelyAllowContact = true;

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    Require(saved.find("PalmAreaThreshold=77") != std::string::npos,
            "old palm area key should be saved");
    Require(saved.find("PeakEvalFingerProminence=333") != std::string::npos,
            "new peak evaluator key should be saved");

    Solvers::TouchPipeline loaded;
    LoadFromSavedText(loaded, saved);

    Require(!loaded.m_palmReject.m_enabled, "old PalmEnabled key should round-trip");
    Require(loaded.m_palmReject.m_areaThreshold == 77, "old palm area key should round-trip");
    Require(loaded.m_palmReject.m_signalSumThreshold == 123456, "old palm signal key should round-trip");
    RequireNear(loaded.m_palmReject.m_densityThresholdLow, 321.5f, 0.0001f,
                "old palm density key should round-trip");
    Require(!loaded.m_palmReject.m_elongatedEnabled, "old palm elongated key should round-trip");
    Require(!loaded.m_palmReject.m_analyzerEnabled, "new analyzer enabled key should round-trip");
    Require(loaded.m_palmReject.m_areaMinForDensity == 22, "old palm density min area key should round-trip");
    Require(loaded.m_palmReject.m_elongatedMinArea == 12, "old palm elongated min area key should round-trip");
    RequireNear(loaded.m_palmReject.m_elongatedAspectRatio, 3.25f, 0.0001f,
                "old palm elongated aspect key should round-trip");
    Require(loaded.m_palmReject.m_candidateAreaThreshold == 44, "new candidate area key should round-trip");
    Require(loaded.m_palmReject.m_candidateSignalThreshold == 65432, "new candidate signal key should round-trip");
    Require(loaded.m_palmReject.m_likelyAreaThreshold == 66, "new likely area key should round-trip");
    RequireNear(loaded.m_palmReject.m_fillRatioThreshold, 0.55f, 0.0001f,
                "new fill ratio key should round-trip");
    RequireNear(loaded.m_palmReject.m_flatSharpnessThreshold, 1.22f, 0.0001f,
                "new flat sharpness key should round-trip");
    Require(loaded.m_palmReject.m_strongPeakProminence == 222, "new strong peak key should round-trip");
    Require(!loaded.m_peakEval.m_enabled, "new peak evaluator enabled key should round-trip");
    Require(loaded.m_peakEval.m_fingerProminence == 333, "new peak evaluator prominence key should round-trip");
    RequireNear(loaded.m_peakEval.m_fingerSharpness, 1.77f, 0.0001f,
                "new peak evaluator sharpness key should round-trip");
    RequireNear(loaded.m_peakEval.m_palmSharpnessMax, 1.11f, 0.0001f,
                "new peak evaluator palm max key should round-trip");
    RequireNear(loaded.m_peakEval.m_ambiguousMargin, 0.25f, 0.0001f,
                "new peak evaluator ambiguous key should round-trip");
    Require(!loaded.m_peakEval.m_palmAwareExpansionEnabled, "new palm-aware expansion key should round-trip");
    RequireNear(loaded.m_peakEval.m_fingerInPalmThresholdRatio, 0.8f, 0.0001f,
                "new finger-in-palm threshold key should round-trip");
    Require(loaded.m_peakEval.m_fingerInPalmMaxRadius == 4, "new finger-in-palm radius key should round-trip");
    Require(loaded.m_peakEval.m_palmLikelyAllowContact, "new palm allow contact key should round-trip");
}

} // namespace

int main() {
    try {
        TestPalmRejectorPreservesMacroZone();
        TestPeakEvaluatorKeepsSharpPeakInPalmCandidate();
        TestPeakEvaluatorSuppressesFlatPalmPeak();
        TestPeakEvaluatorSuppressesBroadPalmPressurePeak();
        TestZoneExpanderSkipsPalmLikelyPeak();
        TestPalmConfigRoundTrip();
        std::cout << "[TEST] Touch palm rejection MVP tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
