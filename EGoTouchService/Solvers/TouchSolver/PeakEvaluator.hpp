#pragma once

#include "SolverTypes.h"
#include "PeakDetector.hpp"
#include "PalmTypes.hpp"
#include <algorithm>
#include <span>
#include <vector>

namespace Solvers { namespace Touch {

class PeakEvaluator {
public:
    bool  m_enabled = true;
    int   m_fingerProminence = 100;
    float m_fingerSharpness = 3.35f;
    float m_palmSharpnessMax = 3.30f;
    float m_ambiguousMargin = 0.15f;
    bool  m_palmAwareExpansionEnabled = true;
    float m_fingerInPalmThresholdRatio = 0.70f;
    int   m_fingerInPalmMaxRadius = 3;
    bool  m_palmLikelyAllowContact = false;

    inline void Process(const HeatmapFrame& frame,
                        std::span<const Peak> peaks,
                        const std::vector<MacroZoneFeature>& zoneFeatures) {
        m_evaluations.clear();
        if (!m_enabled) return;
        m_evaluations.resize(peaks.size());

        for (size_t i = 0; i < peaks.size(); ++i) {
            m_evaluations[i] = EvaluatePeak(frame, peaks[i], zoneFeatures);
        }
    }

    std::span<const PeakEvaluation> GetEvaluations() const {
        return std::span<const PeakEvaluation>(m_evaluations.data(), m_evaluations.size());
    }

private:
    static constexpr int kRows = 40;
    static constexpr int kCols = 60;

    std::vector<PeakEvaluation> m_evaluations;

    inline PeakEvaluation EvaluatePeak(const HeatmapFrame& frame,
                                       const Peak& peak,
                                       const std::vector<MacroZoneFeature>& zoneFeatures) const {
        PeakEvaluation eval;
        const MacroZoneFeature* zone = nullptr;
        if (peak.macroZoneIndex >= 0 && peak.macroZoneIndex < static_cast<int>(zoneFeatures.size())) {
            zone = &zoneFeatures[static_cast<size_t>(peak.macroZoneIndex)];
            eval.zonePalmClass = zone->palmClass;
        }

        eval.localMean3x3 = LocalMean(frame, peak.r, peak.c, 1);
        eval.localMean5x5 = LocalMean(frame, peak.r, peak.c, 2);
        eval.prominence = static_cast<float>(peak.z) - eval.localMean5x5;
        eval.sharpness = static_cast<float>(peak.z) / std::max(1.0f, eval.localMean5x5);

        const bool inPalmZone = eval.zonePalmClass == PalmClass::PalmCandidate ||
                                eval.zonePalmClass == PalmClass::PalmLikely;
        const bool strongFingerShape = peak.z >= 1 &&
                                       eval.prominence >= static_cast<float>(m_fingerProminence) &&
                                       eval.sharpness >= m_fingerSharpness;
        const bool flatPalmShape = inPalmZone && eval.sharpness <= m_palmSharpnessMax;

        float fingerScore = 0.0f;
        if (eval.prominence >= static_cast<float>(m_fingerProminence)) fingerScore += 0.45f;
        if (eval.sharpness >= m_fingerSharpness) fingerScore += 0.35f;
        if (zone && zone->palmClass == PalmClass::FingerLikely) fingerScore += 0.20f;
        if (inPalmZone && strongFingerShape) fingerScore += 0.15f;
        eval.fingerScore = std::clamp(fingerScore, 0.0f, 1.0f);

        float palmScore = 0.0f;
        if (zone) palmScore += zone->palmScore * 0.45f;
        if (flatPalmShape) palmScore += 0.45f;
        if (inPalmZone && !strongFingerShape) palmScore += 0.15f;
        eval.palmScore = std::clamp(palmScore, 0.0f, 1.0f);

        if (strongFingerShape && eval.fingerScore + m_ambiguousMargin >= eval.palmScore) {
            eval.palmClass = PalmClass::FingerLikely;
        } else if (flatPalmShape && eval.palmScore > eval.fingerScore) {
            eval.palmClass = PalmClass::PalmLikely;
        } else if (inPalmZone) {
            eval.palmClass = PalmClass::Ambiguous;
        } else if (eval.fingerScore >= 0.45f) {
            eval.palmClass = PalmClass::FingerLikely;
        } else {
            eval.palmClass = PalmClass::Unknown;
        }

        eval.allowContact = !m_enabled || eval.palmClass != PalmClass::PalmLikely || m_palmLikelyAllowContact;
        eval.palmEvidenceOnly = m_enabled && eval.palmClass == PalmClass::PalmLikely && !eval.allowContact;
        if (strongFingerShape) eval.evalFlags |= PalmReasonStrongSharpPeakPresent;
        if (flatPalmShape) eval.evalFlags |= PalmReasonFlatSignalShape;
        return eval;
    }

    inline float LocalMean(const HeatmapFrame& frame, int row, int col, int radius) const {
        int sum = 0;
        int count = 0;
        for (int dr = -radius; dr <= radius; ++dr) {
            for (int dc = -radius; dc <= radius; ++dc) {
                const int r = row + dr;
                const int c = col + dc;
                if (r < 0 || r >= kRows || c < 0 || c >= kCols) continue;
                sum += frame.heatmapMatrix[r][c];
                ++count;
            }
        }
        return count > 0 ? static_cast<float>(sum) / static_cast<float>(count) : 0.0f;
    }
};

}} // namespace Solvers::Touch
