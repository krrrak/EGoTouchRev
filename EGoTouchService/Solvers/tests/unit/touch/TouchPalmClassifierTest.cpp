#include "TouchSolver/TouchClassifier.hpp"
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

void TestTouchClassifierPreservesMacroZone() {
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

    Solvers::Touch::TouchClassifier rejector;
    rejector.m_areaThreshold = 10;
    rejector.m_signalSumThreshold = 1000;
    const int rejected = rejector.AnalyzeZones(frame, zones);

    Require(rejected == 0, "TouchClassifier should no longer reject zones early");
    Require(zones.size() == 1, "TouchClassifier should preserve palm-like MacroZone");
    Require(rejector.GetZoneFeatures().size() == 1, "TouchClassifier should record zone features");
    Require(rejector.GetZoneFeatures()[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "large preserved MacroZone should be classified as palm likely");
}

void TestTouchClassifierKeepsSharpPeakInPalmCandidate() {
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

    Solvers::Touch::TouchClassifier evaluator;
    evaluator.EvaluatePeaks(frame, peaks, zones);
    const auto evals = evaluator.GetEvaluations();

    Require(evals.size() == 1, "TouchClassifier should emit one evaluation per peak");
    Require(evals[0].palmClass == Solvers::Touch::PalmClass::FingerLikely,
            "sharp peak in palm candidate should be finger likely");
    Require(evals[0].allowContact, "finger-like peak in palm candidate should remain eligible");
}

void TestTouchClassifierSuppressesFlatPalmPeak() {
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

    Solvers::Touch::TouchClassifier evaluator;
    evaluator.EvaluatePeaks(frame, peaks, zones);
    const auto evals = evaluator.GetEvaluations();

    Require(evals[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "flat peak in palm-like zone should be palm likely");
    Require(!evals[0].allowContact, "palm likely peak should not become contact peak");
    Require(evals[0].palmEvidenceOnly, "suppressed palm peak should remain palm evidence");
}

void TestTouchClassifierSuppressesBroadPalmPressurePeak() {
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

    Solvers::Touch::TouchClassifier evaluator;
    evaluator.EvaluatePeaks(frame, peaks, zones);
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

void TestPalmShadowSuppressesAdjacentFragment() {
    Solvers::Touch::TouchClassifier classifier;
    classifier.m_palmShadowRadius = 2;
    classifier.m_palmShadowHoldFrames = 12;

    Solvers::HeatmapFrame palmFrame;
    std::vector<int> palmPixels;
    for (int r = 10; r < 18; ++r) {
        for (int c = 10; c < 18; ++c) {
            palmPixels.push_back(r * 60 + c);
            palmFrame.heatmapMatrix[r][c] = 1500;
        }
    }
    auto palmZone = MakeZone(palmPixels, 10, 17, 10, 17);
    palmZone.signalSum = static_cast<int>(palmPixels.size()) * 1500;
    std::vector<Solvers::MacroZone> palmZones{palmZone};
    std::vector<Solvers::Touch::Peak> noPeaks;
    classifier.Process(palmFrame, palmZones, noPeaks);

    Solvers::HeatmapFrame fragmentFrame;
    std::vector<int> fragmentPixels;
    for (int r = 18; r <= 19; ++r) {
        for (int c = 18; c <= 19; ++c) {
            fragmentPixels.push_back(r * 60 + c);
            fragmentFrame.heatmapMatrix[r][c] = 700;
        }
    }
    fragmentFrame.heatmapMatrix[18][18] = 900;
    auto fragmentZone = MakeZone(fragmentPixels, 18, 19, 18, 19);
    fragmentZone.signalSum = 3000;
    std::vector<Solvers::MacroZone> fragmentZones{fragmentZone};

    Solvers::Touch::Peak peak;
    peak.r = 18;
    peak.c = 18;
    peak.z = 900;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    classifier.Process(fragmentFrame, fragmentZones, peaks);
    const auto& features = classifier.GetZoneFeatures();
    const auto evals = classifier.GetEvaluations();

    Require(features.size() == 1, "fragment should produce one zone feature");
    Require((features[0].reasonFlags & Solvers::Touch::PalmReasonShadowTouch) != 0,
            "fragment touching palm shadow should be flagged");
    Require(evals.size() == 1, "fragment should produce one peak evaluation");
    Require(evals[0].palmClass == Solvers::Touch::PalmClass::PalmLikely,
            "fragment touching palm shadow should be palm likely");
    Require(!evals[0].allowContact, "fragment touching palm shadow should not create contact");
}

void TestPalmShadowExpiresAfterHoldFrames() {
    Solvers::Touch::TouchClassifier classifier;
    classifier.m_palmShadowRadius = 1;
    classifier.m_palmShadowHoldFrames = 2;

    Solvers::HeatmapFrame palmFrame;
    std::vector<int> palmPixels;
    for (int r = 10; r < 18; ++r) {
        for (int c = 10; c < 18; ++c) {
            palmPixels.push_back(r * 60 + c);
            palmFrame.heatmapMatrix[r][c] = 1500;
        }
    }
    auto palmZone = MakeZone(palmPixels, 10, 17, 10, 17);
    palmZone.signalSum = static_cast<int>(palmPixels.size()) * 1500;
    std::vector<Solvers::MacroZone> palmZones{palmZone};
    std::vector<Solvers::Touch::Peak> noPeaks;
    classifier.Process(palmFrame, palmZones, noPeaks);

    Solvers::HeatmapFrame emptyFrame;
    std::vector<Solvers::MacroZone> emptyZones;
    classifier.Process(emptyFrame, emptyZones, noPeaks);
    classifier.Process(emptyFrame, emptyZones, noPeaks);

    Solvers::HeatmapFrame fragmentFrame;
    fragmentFrame.heatmapMatrix[18][18] = 900;
    std::vector<int> fragmentPixels{18 * 60 + 18};
    auto fragmentZone = MakeZone(fragmentPixels, 18, 18, 18, 18);
    fragmentZone.signalSum = 900;
    std::vector<Solvers::MacroZone> fragmentZones{fragmentZone};

    Solvers::Touch::Peak peak;
    peak.r = 18;
    peak.c = 18;
    peak.z = 900;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    classifier.Process(fragmentFrame, fragmentZones, peaks);
    const auto& features = classifier.GetZoneFeatures();
    const auto evals = classifier.GetEvaluations();

    Require(features.size() == 1, "expired fragment should produce one zone feature");
    Require((features[0].reasonFlags & Solvers::Touch::PalmReasonShadowTouch) == 0,
            "expired palm shadow should not suppress later fragment");
    Require(evals.size() == 1, "expired fragment should produce one peak evaluation");
    Require(evals[0].allowContact, "expired palm shadow should allow finger-like fragment contact");
}

void TestPalmShadowUsesRealPixelsNotBoundingBox() {
    Solvers::Touch::TouchClassifier classifier;
    classifier.m_palmShadowRadius = 1;
    classifier.m_palmShadowHoldFrames = 12;
    classifier.m_areaThreshold = 30;
    classifier.m_candidateAreaThreshold = 20;
    classifier.m_likelyAreaThreshold = 30;
    classifier.m_signalSumThreshold = 50000;
    classifier.m_candidateSignalThreshold = 50000;

    Solvers::HeatmapFrame palmFrame;
    std::vector<int> palmPixels;
    for (int c = 10; c <= 20; ++c) {
        palmPixels.push_back(10 * 60 + c);
        palmPixels.push_back(20 * 60 + c);
        palmFrame.heatmapMatrix[10][c] = 1500;
        palmFrame.heatmapMatrix[20][c] = 1500;
    }
    for (int r = 11; r <= 19; ++r) {
        palmPixels.push_back(r * 60 + 10);
        palmPixels.push_back(r * 60 + 20);
        palmFrame.heatmapMatrix[r][10] = 1500;
        palmFrame.heatmapMatrix[r][20] = 1500;
    }
    auto palmZone = MakeZone(palmPixels, 10, 20, 10, 20);
    palmZone.signalSum = static_cast<int>(palmPixels.size()) * 1500;
    std::vector<Solvers::MacroZone> palmZones{palmZone};
    std::vector<Solvers::Touch::Peak> noPeaks;
    classifier.Process(palmFrame, palmZones, noPeaks);

    Solvers::HeatmapFrame islandFrame;
    islandFrame.heatmapMatrix[15][15] = 900;
    std::vector<int> islandPixels{15 * 60 + 15};
    auto islandZone = MakeZone(islandPixels, 15, 15, 15, 15);
    islandZone.signalSum = 900;
    std::vector<Solvers::MacroZone> islandZones{islandZone};

    Solvers::Touch::Peak peak;
    peak.r = 15;
    peak.c = 15;
    peak.z = 900;
    peak.macroZoneIndex = 0;
    std::vector<Solvers::Touch::Peak> peaks{peak};

    classifier.Process(islandFrame, islandZones, peaks);
    const auto& features = classifier.GetZoneFeatures();
    const auto evals = classifier.GetEvaluations();

    Require(features.size() == 1, "bbox-only island should produce one zone feature");
    Require((features[0].reasonFlags & Solvers::Touch::PalmReasonShadowTouch) == 0,
            "palm shadow should use real pixels instead of bounding box fill");
    Require(evals.size() == 1, "bbox-only island should produce one peak evaluation");
    Require(evals[0].allowContact, "bbox-only island should not be suppressed by real-pixel shadow");
}

void TestPalmConfigRoundTrip() {
    Solvers::TouchPipeline pipeline;
    pipeline.m_touchClassifier.m_enabled = false;
    pipeline.m_touchClassifier.m_areaThreshold = 77;
    pipeline.m_touchClassifier.m_signalSumThreshold = 123456;
    pipeline.m_touchClassifier.m_densityThresholdLow = 321.5f;
    pipeline.m_touchClassifier.m_areaMinForDensity = 22;
    pipeline.m_touchClassifier.m_elongatedEnabled = false;
    pipeline.m_touchClassifier.m_elongatedMinArea = 12;
    pipeline.m_touchClassifier.m_elongatedAspectRatio = 3.25f;
    pipeline.m_touchClassifier.m_analyzerEnabled = false;
    pipeline.m_touchClassifier.m_candidateAreaThreshold = 44;
    pipeline.m_touchClassifier.m_candidateSignalThreshold = 65432;
    pipeline.m_touchClassifier.m_likelyAreaThreshold = 66;
    pipeline.m_touchClassifier.m_fillRatioThreshold = 0.55f;
    pipeline.m_touchClassifier.m_flatSharpnessThreshold = 1.22f;
    pipeline.m_touchClassifier.m_strongPeakProminence = 222;
    pipeline.m_touchClassifier.m_peakEvalEnabled = false;
    pipeline.m_touchClassifier.m_fingerProminence = 333;
    pipeline.m_touchClassifier.m_fingerSharpness = 1.77f;
    pipeline.m_touchClassifier.m_palmSharpnessMax = 1.11f;
    pipeline.m_touchClassifier.m_ambiguousMargin = 0.25f;
    pipeline.m_touchClassifier.m_palmAwareExpansionEnabled = false;
    pipeline.m_touchClassifier.m_fingerInPalmThresholdRatio = 0.8f;
    pipeline.m_touchClassifier.m_fingerInPalmMaxRadius = 4;
    pipeline.m_touchClassifier.m_palmLikelyAllowContact = true;
    pipeline.m_touchClassifier.m_palmShadowEnabled = false;
    pipeline.m_touchClassifier.m_palmShadowRadius = 6;
    pipeline.m_touchClassifier.m_palmShadowHoldFrames = 33;
    pipeline.m_touchClassifier.m_palmShadowSeedScore = 0.72f;

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    const char* frozenSerializedKeys[] = {
        "PalmEnabled=",
        "PalmAreaThreshold=",
        "PalmSignalSumThreshold=",
        "PalmDensityThresholdLow=",
        "PalmAreaMinForDensity=",
        "PalmElongatedEnabled=",
        "PalmElongatedMinArea=",
        "PalmElongatedAspectRatio=",
        "PalmAnalyzerEnabled=",
        "PalmCandidateAreaThreshold=",
        "PalmCandidateSignalThreshold=",
        "PalmLikelyAreaThreshold=",
        "PalmFillRatioThreshold=",
        "PalmFlatSharpnessThreshold=",
        "PalmStrongPeakProminence=",
        "PeakEvalEnabled=",
        "PeakEvalFingerProminence=",
        "PeakEvalFingerSharpness=",
        "PeakEvalPalmSharpnessMax=",
        "PeakEvalAmbiguousMargin=",
        "PalmAwareExpansionEnabled=",
        "PalmFingerInPalmThresholdRatio=",
        "PalmFingerInPalmMaxRadius=",
        "PalmLikelyAllowContact=",
        "PalmShadowEnabled=",
        "PalmShadowRadius=",
        "PalmShadowHoldFrames=",
        "PalmShadowSeedScore=",
    };
    for (const char* key : frozenSerializedKeys) {
        Require(saved.find(key) == std::string::npos,
                "frozen palm classifier key should not be saved");
    }

    Solvers::TouchPipeline loaded;
    const auto defaults = loaded.m_touchClassifier;
    LoadFromSavedText(loaded, saved);

    loaded.LoadConfig("PalmEnabled", "0");
    loaded.LoadConfig("PalmAreaThreshold", "77");
    loaded.LoadConfig("PalmSignalSumThreshold", "123456");
    loaded.LoadConfig("PalmDensityThresholdLow", "321.5");
    loaded.LoadConfig("PalmAreaMinForDensity", "22");
    loaded.LoadConfig("PalmElongatedEnabled", "0");
    loaded.LoadConfig("PalmElongatedMinArea", "12");
    loaded.LoadConfig("PalmElongatedAspectRatio", "3.25");
    loaded.LoadConfig("PalmAnalyzerEnabled", "0");
    loaded.LoadConfig("PalmCandidateAreaThreshold", "44");
    loaded.LoadConfig("PalmCandidateSignalThreshold", "65432");
    loaded.LoadConfig("PalmLikelyAreaThreshold", "66");
    loaded.LoadConfig("PalmFillRatioThreshold", "0.55");
    loaded.LoadConfig("PalmFlatSharpnessThreshold", "1.22");
    loaded.LoadConfig("PalmStrongPeakProminence", "222");
    loaded.LoadConfig("PeakEvalEnabled", "0");
    loaded.LoadConfig("PeakEvalFingerProminence", "333");
    loaded.LoadConfig("PeakEvalFingerSharpness", "1.77");
    loaded.LoadConfig("PeakEvalPalmSharpnessMax", "1.11");
    loaded.LoadConfig("PeakEvalAmbiguousMargin", "0.25");
    loaded.LoadConfig("PalmAwareExpansionEnabled", "0");
    loaded.LoadConfig("PalmFingerInPalmThresholdRatio", "0.8");
    loaded.LoadConfig("PalmFingerInPalmMaxRadius", "4");
    loaded.LoadConfig("PalmLikelyAllowContact", "1");
    loaded.LoadConfig("PalmShadowEnabled", "0");
    loaded.LoadConfig("PalmShadowRadius", "6");
    loaded.LoadConfig("PalmShadowHoldFrames", "33");
    loaded.LoadConfig("PalmShadowSeedScore", "0.72");

    Require(loaded.m_touchClassifier.m_enabled == defaults.m_enabled,
            "frozen PalmEnabled key should not load");
    Require(loaded.m_touchClassifier.m_areaThreshold == defaults.m_areaThreshold,
            "frozen palm area key should not load");
    Require(loaded.m_touchClassifier.m_signalSumThreshold == defaults.m_signalSumThreshold,
            "frozen palm signal key should not load");
    RequireNear(loaded.m_touchClassifier.m_densityThresholdLow, defaults.m_densityThresholdLow, 0.0001f,
                "frozen palm density key should not load");
    Require(loaded.m_touchClassifier.m_areaMinForDensity == defaults.m_areaMinForDensity,
            "frozen palm density min area key should not load");
    Require(loaded.m_touchClassifier.m_elongatedEnabled == defaults.m_elongatedEnabled,
            "frozen palm elongated key should not load");
    Require(loaded.m_touchClassifier.m_elongatedMinArea == defaults.m_elongatedMinArea,
            "frozen palm elongated min area key should not load");
    RequireNear(loaded.m_touchClassifier.m_elongatedAspectRatio, defaults.m_elongatedAspectRatio, 0.0001f,
                "frozen palm elongated aspect key should not load");
    Require(loaded.m_touchClassifier.m_analyzerEnabled == defaults.m_analyzerEnabled,
            "frozen analyzer enabled key should not load");
    Require(loaded.m_touchClassifier.m_candidateAreaThreshold == defaults.m_candidateAreaThreshold,
            "frozen candidate area key should not load");
    Require(loaded.m_touchClassifier.m_candidateSignalThreshold == defaults.m_candidateSignalThreshold,
            "frozen candidate signal key should not load");
    Require(loaded.m_touchClassifier.m_likelyAreaThreshold == defaults.m_likelyAreaThreshold,
            "frozen likely area key should not load");
    RequireNear(loaded.m_touchClassifier.m_fillRatioThreshold, defaults.m_fillRatioThreshold, 0.0001f,
                "frozen fill ratio key should not load");
    RequireNear(loaded.m_touchClassifier.m_flatSharpnessThreshold, defaults.m_flatSharpnessThreshold, 0.0001f,
                "frozen flat sharpness key should not load");
    Require(loaded.m_touchClassifier.m_strongPeakProminence == defaults.m_strongPeakProminence,
            "frozen strong peak key should not load");
    Require(loaded.m_touchClassifier.m_peakEvalEnabled == defaults.m_peakEvalEnabled,
            "frozen peak evaluator enabled key should not load");
    Require(loaded.m_touchClassifier.m_fingerProminence == defaults.m_fingerProminence,
            "frozen peak evaluator prominence key should not load");
    RequireNear(loaded.m_touchClassifier.m_fingerSharpness, defaults.m_fingerSharpness, 0.0001f,
                "frozen peak evaluator sharpness key should not load");
    RequireNear(loaded.m_touchClassifier.m_palmSharpnessMax, defaults.m_palmSharpnessMax, 0.0001f,
                "frozen peak evaluator palm max key should not load");
    RequireNear(loaded.m_touchClassifier.m_ambiguousMargin, defaults.m_ambiguousMargin, 0.0001f,
                "frozen peak evaluator ambiguous key should not load");
    Require(loaded.m_touchClassifier.m_palmAwareExpansionEnabled == defaults.m_palmAwareExpansionEnabled,
            "frozen palm-aware expansion key should not load");
    RequireNear(loaded.m_touchClassifier.m_fingerInPalmThresholdRatio,
                defaults.m_fingerInPalmThresholdRatio,
                0.0001f,
                "frozen finger-in-palm threshold key should not load");
    Require(loaded.m_touchClassifier.m_fingerInPalmMaxRadius == defaults.m_fingerInPalmMaxRadius,
            "frozen finger-in-palm radius key should not load");
    Require(loaded.m_touchClassifier.m_palmLikelyAllowContact == defaults.m_palmLikelyAllowContact,
            "frozen palm allow contact key should not load");
    Require(loaded.m_touchClassifier.m_palmShadowEnabled == defaults.m_palmShadowEnabled,
            "frozen palm shadow enabled key should not load");
    Require(loaded.m_touchClassifier.m_palmShadowRadius == defaults.m_palmShadowRadius,
            "frozen palm shadow radius key should not load");
    Require(loaded.m_touchClassifier.m_palmShadowHoldFrames == defaults.m_palmShadowHoldFrames,
            "frozen palm shadow hold key should not load");
    RequireNear(loaded.m_touchClassifier.m_palmShadowSeedScore, defaults.m_palmShadowSeedScore, 0.0001f,
                "frozen palm shadow seed score key should not load");
}

} // namespace

int main() {
    try {
        TestTouchClassifierPreservesMacroZone();
        TestTouchClassifierKeepsSharpPeakInPalmCandidate();
        TestTouchClassifierSuppressesFlatPalmPeak();
        TestTouchClassifierSuppressesBroadPalmPressurePeak();
        TestZoneExpanderSkipsPalmLikelyPeak();
        TestPalmShadowSuppressesAdjacentFragment();
        TestPalmShadowExpiresAfterHoldFrames();
        TestPalmShadowUsesRealPixelsNotBoundingBox();
        TestPalmConfigRoundTrip();
        std::cout << "[TEST] Touch palm rejection MVP tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
