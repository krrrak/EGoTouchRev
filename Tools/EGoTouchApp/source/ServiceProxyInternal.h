#pragma once

#include "ServiceProxy.h"
#include <optional>
#include <string>
#include <string_view>

namespace App {

extern const std::string kConfigPath;

std::string TrimCopy(std::string_view input);
bool ParseIniKeyValue(std::string_view line, std::string& key, std::string& value);
bool IsLegacyTouchSection(const std::string& section);
std::optional<std::string> MapLegacyTouchKey(const std::string& section, const std::string& key);

TouchPipelineModuleEnableState CaptureTouchPipelineModuleEnableState(
    const Solvers::TouchPipeline& pipeline);
void ApplyTouchPipelineModuleEnableState(
    Solvers::TouchPipeline& pipeline,
    const TouchPipelineModuleEnableState& state);
std::string BuildServiceConfigSection(bool modeFull,
                                      bool autoMode,
                                      bool stylusVhfEnabled,
                                      PenButtonMode penButtonMode = PenButtonMode::OemCustom,
                                      PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly);
std::string BuildTouchPipelineConfigSection(
    const Solvers::TouchPipeline& pipeline,
    const TouchPipelineModuleEnableState* persistedModuleState = nullptr);
std::string BuildStylusPipelineConfigSection(
    const Solvers::StylusPipeline& pipeline);
std::string MergeServiceProxyConfigSections(
    std::string_view existingText,
    std::string_view serviceSection,
    std::string_view touchSection,
    std::string_view stylusSection);

} // namespace App
