#pragma once

#include <cstdint>

namespace Solvers { namespace Touch {

struct Peak {
    int r = 0, c = 0;
    int16_t z = 0;
    int neighborSignalSum = 0;
    uint8_t id = 0;
    int tzAge = 0;
    int macroZoneIndex = -1;
    int macroZoneArea = 0;
    int macroZoneSignalSum = 0;
};

enum class PalmClass : uint8_t {
    Unknown = 0,
    FingerLikely,
    Ambiguous,
    PalmCandidate,
    PalmLikely
};

enum PalmReasonFlags : uint32_t {
    PalmReasonLargeArea = 0x0001,
    PalmReasonLargeSignalSum = 0x0002,
    PalmReasonLowDensity = 0x0004,
    PalmReasonElongated = 0x0008,
    PalmReasonHighFillRatio = 0x0010,
    PalmReasonEdgeWideContact = 0x0020,
    PalmReasonFlatSignalShape = 0x0040,
    PalmReasonStrongSharpPeakPresent = 0x0080,
    PalmReasonPalmBoxSuppressed = 0x0100
};

struct MacroZoneFeature {
    int zoneIndex = -1;
    int area = 0;
    int signalSum = 0;
    float density = 0.0f;
    int bboxW = 0;
    int bboxH = 0;
    int bboxArea = 0;
    float aspectRatio = 1.0f;
    float fillRatio = 0.0f;
    int maxSignal = 0;
    float meanSignal = 0.0f;
    float signalVariance = 0.0f;
    int edgeTouchMask = 0;
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    uint32_t reasonFlags = 0;
};

struct PeakEvaluation {
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    bool allowContact = true;
    bool palmEvidenceOnly = false;
    int mergeTarget = -1;
    float localMean3x3 = 0.0f;
    float localMean5x5 = 0.0f;
    float prominence = 0.0f;
    float sharpness = 0.0f;
    PalmClass zonePalmClass = PalmClass::Unknown;
    uint32_t evalFlags = 0;
};

}} // namespace Solvers::Touch
