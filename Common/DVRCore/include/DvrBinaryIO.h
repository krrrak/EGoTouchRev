#pragma once

#include "DvrFrameSlot.h"
#include "DvrTypes.h"
#include "SolverTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Dvr {

std::filesystem::path ResolveReplayBinaryPath(const std::filesystem::path& input);

bool WriteBinaryFile(const std::filesystem::path& filePath,
                     const std::vector<DvrFrameSlot>& frames,
                     const DynamicDebugSchema* dynamicSchema = nullptr,
                     const std::vector<DvrDynamicDebugFrameSlot>* dynamicFrames = nullptr,
                     const RuntimeConfigSnapshot* runtimeConfig = nullptr,
                     uint32_t* outFlags = nullptr);

bool ReadBinaryFile(const std::filesystem::path& filePath,
                    std::vector<Solvers::HeatmapFrame>& outFrames,
                    int& outVersion,
                    uint32_t* outFlags = nullptr,
                    std::string* outError = nullptr,
                    DynamicDebugSchema* outDynamicSchema = nullptr,
                    std::vector<DynamicDebugFrame>* outDynamicFrames = nullptr,
                    RuntimeConfigSnapshot* outRuntimeConfig = nullptr);

} // namespace Dvr

namespace App {

inline std::filesystem::path ResolveReplayBinaryPath(const std::filesystem::path& input) {
    return Dvr::ResolveReplayBinaryPath(input);
}

inline bool WriteDvrBinaryFile(const std::filesystem::path& filePath,
                               const std::vector<Dvr::DvrFrameSlot>& frames,
                               const DvrDynamicDebugSchema* dynamicSchema = nullptr,
                               const std::vector<Dvr::DvrDynamicDebugFrameSlot>* dynamicFrames = nullptr,
                               const DvrRuntimeConfigSnapshot* runtimeConfig = nullptr,
                               uint32_t* outFlags = nullptr) {
    return Dvr::WriteBinaryFile(filePath, frames, dynamicSchema, dynamicFrames, runtimeConfig, outFlags);
}

inline bool ReadDvrBinaryFile(const std::filesystem::path& filePath,
                              std::vector<Solvers::HeatmapFrame>& outFrames,
                              int& outVersion,
                              uint32_t* outFlags = nullptr,
                              std::string* outError = nullptr,
                              DvrDynamicDebugSchema* outDynamicSchema = nullptr,
                              std::vector<DvrDynamicDebugFrame>* outDynamicFrames = nullptr,
                              DvrRuntimeConfigSnapshot* outRuntimeConfig = nullptr) {
    return Dvr::ReadBinaryFile(filePath, outFrames, outVersion, outFlags, outError, outDynamicSchema, outDynamicFrames, outRuntimeConfig);
}

} // namespace App
