#pragma once

#include "DvrTypes.h"
#include "SolverTypes.h"

#include <filesystem>
#include <string_view>

namespace Solvers {
class TouchPipeline;
}

namespace Dvr {

bool WriteFrameCsvFile(const std::filesystem::path& filePath,
                       const Solvers::HeatmapFrame& frame,
                       const Solvers::TouchPipeline* pipeline,
                       bool includeHeatmap,
                       bool includeMasterStatus,
                       bool includeSlaveStatus,
                       std::string_view captureMode,
                       bool includeMetadataHeader = true,
                       int formatVersion = 0,
                       std::string_view sourceName = {},
                       const DynamicDebugSchema* dynamicSchema = nullptr,
                       const DynamicDebugFrame* dynamicFrame = nullptr,
                       const RuntimeConfigSnapshot* runtimeConfig = nullptr);

} // namespace Dvr

namespace App {

using Dvr::WriteFrameCsvFile;

} // namespace App
