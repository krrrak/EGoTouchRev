#pragma once

#include "ServiceProxy.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App {

extern const std::string kConfigPath;

std::string TrimCopy(std::string_view input);
bool ParseIniKeyValue(std::string_view line, std::string& key, std::string& value);
bool IsLegacyTouchSection(const std::string& section);
std::optional<std::string> MapLegacyTouchKey(const std::string& section, const std::string& key);

std::filesystem::path ResolveReplayBinaryPath(const std::filesystem::path& input);
bool WriteDvrBinaryFile(const std::filesystem::path& filePath,
                        const std::vector<Dvr::DvrFrameSlot>& frames,
                        const DvrDynamicDebugSchema* dynamicSchema = nullptr,
                        uint32_t* outFlags = nullptr);
bool ReadDvrBinaryFile(const std::filesystem::path& filePath,
                       std::vector<Solvers::HeatmapFrame>& outFrames,
                       int& outVersion,
                       uint32_t* outFlags = nullptr,
                       std::string* outError = nullptr,
                       DvrDynamicDebugSchema* outDynamicSchema = nullptr,
                       std::vector<DvrDynamicDebugFrame>* outDynamicFrames = nullptr);
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
                       const DvrDynamicDebugSchema* dynamicSchema = nullptr,
                       const DvrDynamicDebugFrame* dynamicFrame = nullptr);

} // namespace App
