#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "SolverBuildConfig.h"
#include "FrameLayout.h"
#include "TouchFrameTypes.h"
#include "StylusFrameTypes.h"

namespace Solvers {

struct HeatmapFrame {
#if EGOTOUCH_DIAG
    std::vector<uint8_t> rawData;
#endif
    const uint8_t* rawPtr = nullptr;
    size_t rawLen = 0;

    Frame::MasterSuffixView masterSuffix{};
    Frame::SlaveSuffixView slaveSuffix{};
    bool masterSuffixValid = false;
    bool slaveSuffixValid = false;

    int16_t heatmapMatrix[40][60];

    TouchFrameData touch;
    StylusFrameData stylus;

    uint64_t timestamp;
    uint64_t receiveSystemEpochUs = 0;
    bool masterWasRead = true;

    HeatmapFrame() : timestamp(0) {
        // TODO(perf): This 4800B clear is redundant for complete parsed frames, but
        // MasterFrameParser can be disabled or skip short frames without overwriting.
        std::memset(heatmapMatrix, 0, sizeof(heatmapMatrix));
    }
};

} // namespace Solvers
