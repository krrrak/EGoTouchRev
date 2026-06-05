#include "ConfigUIRenderer.h"
#include "config/ConfigStore.h"
#include "imgui.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace App {

namespace {

template <typename UInt>
void RenderUnsignedInt(const std::string& label,
                       const Solvers::ConfigParam& param,
                       int typeMax) {
    auto* ptr = static_cast<UInt*>(param.valuePtr);
    int value = static_cast<int>(*ptr);
    const bool hasRange = param.maxVal > param.minVal;
    const int minValue = hasRange ? static_cast<int>(param.minVal) : 0;
    const int maxValue = hasRange ? static_cast<int>(param.maxVal) : typeMax;

    if (hasRange) {
        if (ImGui::SliderInt(label.c_str(), &value, minValue, maxValue)) {
            *ptr = static_cast<UInt>(std::clamp(value, minValue, maxValue));
        }
    } else if (ImGui::InputInt(label.c_str(), &value)) {
        *ptr = static_cast<UInt>(std::clamp(value, minValue, maxValue));
    }
}

} // namespace

void ConfigUIRenderer::RenderConfigSchema(
    const std::vector<Solvers::ConfigParam>& schema,
    const std::string& sectionName,
    std::optional<Solvers::ConfigParam::Category> filterCategory) {

    if (schema.empty()) return;

    for (const auto& param : schema) {
        if (filterCategory.has_value() && param.category != filterCategory.value()) {
            continue;
        }

        // Appending ## makes the ID unique in ImGui without rendering the suffix
        std::string label = param.displayName + "##" + sectionName + "_" + param.key;
        
        switch (param.type) {
            case Solvers::ConfigParam::Bool:
                ImGui::Checkbox(label.c_str(),
                               static_cast<bool*>(param.valuePtr));
                break;

            case Solvers::ConfigParam::Int: {
                int* ptr = static_cast<int*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    if (ImGui::SliderInt(label.c_str(), ptr,
                                         static_cast<int>(param.minVal),
                                         static_cast<int>(param.maxVal))) {
                        *ptr = std::clamp(*ptr, static_cast<int>(param.minVal), static_cast<int>(param.maxVal));
                    }
                } else {
                    ImGui::InputInt(label.c_str(), ptr);
                }
                break;
            }

            case Solvers::ConfigParam::UInt8:
                RenderUnsignedInt<std::uint8_t>(label, param, 0xFF);
                break;

            case Solvers::ConfigParam::UInt16:
                RenderUnsignedInt<std::uint16_t>(label, param, 0xFFFF);
                break;

            case Solvers::ConfigParam::UInt32:
                RenderUnsignedInt<std::uint32_t>(label, param, 0x7FFFFFFF);
                break;

            case Solvers::ConfigParam::Float: {
                float* ptr = static_cast<float*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    if (ImGui::SliderFloat(label.c_str(), ptr, param.minVal, param.maxVal)) {
                        *ptr = std::clamp(*ptr, param.minVal, param.maxVal);
                    }
                } else {
                    ImGui::InputFloat(label.c_str(), ptr);
                }
                break;
            }

            case Solvers::ConfigParam::Double: {
                double* ptr = static_cast<double*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    float fval = static_cast<float>(*ptr);
                    if (ImGui::SliderFloat(label.c_str(), &fval, param.minVal, param.maxVal)) {
                        fval = std::clamp(fval, param.minVal, param.maxVal);
                        *ptr = static_cast<double>(fval);
                    }
                } else {
                    float fval = static_cast<float>(*ptr);
                    if (ImGui::InputFloat(label.c_str(), &fval)) {
                        *ptr = static_cast<double>(fval);
                    }
                }
                break;
            }

            case Solvers::ConfigParam::String:
                // String support can be added if needed
                ImGui::Text("%s: (string)", label.c_str());
                break;
        }
    }
}

void ConfigUIRenderer::RenderConfigSchemaByModule(
    const std::vector<Solvers::ConfigParam>& schema,
    const std::string& moduleTag) {

    for (const auto& param : schema) {
        if (param.moduleTag != moduleTag) continue;

        std::string label = param.displayName + "##mod_" + moduleTag + "_" + param.key;

        switch (param.type) {
            case Solvers::ConfigParam::Bool:
                ImGui::Checkbox(label.c_str(),
                               static_cast<bool*>(param.valuePtr));
                break;

            case Solvers::ConfigParam::Int: {
                int* ptr = static_cast<int*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    if (ImGui::SliderInt(label.c_str(), ptr,
                                         static_cast<int>(param.minVal),
                                         static_cast<int>(param.maxVal))) {
                        *ptr = std::clamp(*ptr, static_cast<int>(param.minVal), static_cast<int>(param.maxVal));
                    }
                } else {
                    ImGui::InputInt(label.c_str(), ptr);
                }
                break;
            }

            case Solvers::ConfigParam::UInt8:
                RenderUnsignedInt<std::uint8_t>(label, param, 0xFF);
                break;

            case Solvers::ConfigParam::UInt16:
                RenderUnsignedInt<std::uint16_t>(label, param, 0xFFFF);
                break;

            case Solvers::ConfigParam::UInt32:
                RenderUnsignedInt<std::uint32_t>(label, param, 0x7FFFFFFF);
                break;

            case Solvers::ConfigParam::Float: {
                float* ptr = static_cast<float*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    if (ImGui::SliderFloat(label.c_str(), ptr, param.minVal, param.maxVal)) {
                        *ptr = std::clamp(*ptr, param.minVal, param.maxVal);
                    }
                } else {
                    ImGui::InputFloat(label.c_str(), ptr);
                }
                break;
            }

            case Solvers::ConfigParam::Double: {
                double* ptr = static_cast<double*>(param.valuePtr);
                if (param.maxVal > param.minVal) {
                    float fval = static_cast<float>(*ptr);
                    if (ImGui::SliderFloat(label.c_str(), &fval, param.minVal, param.maxVal)) {
                        fval = std::clamp(fval, param.minVal, param.maxVal);
                        *ptr = static_cast<double>(fval);
                    }
                } else {
                    float fval = static_cast<float>(*ptr);
                    if (ImGui::InputFloat(label.c_str(), &fval)) {
                        *ptr = static_cast<double>(fval);
                    }
                }
                break;
            }

            case Solvers::ConfigParam::String:
                ImGui::Text("%s: (string)", label.c_str());
                break;
        }
    }
}

void ConfigUIRenderer::RenderConfigStore(
    const Config::ConfigSchemaSnapshot& schema,
    Config::ConfigStore& values,
    const std::string& sectionName) {

    (void)sectionName;

    for (const auto& entry : schema.entries) {
        Config::ConfigValue currentValue;
        if (values.has(entry.yamlPath)) {
            currentValue = values.get<Config::ConfigValue>(entry.yamlPath);
        } else if (entry.uiType != Config::ConfigUiType::String || !Config::toString(entry.currentValue).empty()) {
            currentValue = entry.currentValue;
        } else {
            currentValue = entry.defaultValue;
        }

        const std::string label = entry.displayName + "##" + entry.yamlPath;
        const bool hasRange = entry.range.has_value();

        switch (entry.uiType) {
            case Config::ConfigUiType::Bool: {
                bool val = Config::tryGetValue<bool>(currentValue).value_or(false);
                if (ImGui::Checkbox(label.c_str(), &val)) {
                    values.set<bool>(entry.yamlPath, val);
                }
                break;
            }

            case Config::ConfigUiType::Int32: {
                int val = Config::tryGetValue<int32_t>(currentValue).value_or(0);
                const int minV = hasRange ? static_cast<int>(entry.range->min) : 0;
                const int maxV = hasRange ? static_cast<int>(entry.range->max) : 100;
                if (hasRange) {
                    if (ImGui::SliderInt(label.c_str(), &val, minV, maxV)) {
                        values.set<int32_t>(entry.yamlPath, static_cast<int32_t>(val));
                    }
                } else if (ImGui::InputInt(label.c_str(), &val)) {
                    values.set<int32_t>(entry.yamlPath, static_cast<int32_t>(val));
                }
                break;
            }

            case Config::ConfigUiType::Float: {
                float val = Config::tryGetValue<float>(currentValue).value_or(0.0f);
                const float minF = hasRange ? static_cast<float>(entry.range->min) : 0.0f;
                const float maxF = hasRange ? static_cast<float>(entry.range->max) : 1.0f;
                if (hasRange) {
                    if (ImGui::SliderFloat(label.c_str(), &val, minF, maxF)) {
                        values.set<float>(entry.yamlPath, val);
                    }
                } else if (ImGui::InputFloat(label.c_str(), &val)) {
                    values.set<float>(entry.yamlPath, val);
                }
                break;
            }

            case Config::ConfigUiType::Enum: {
                auto strVal = Config::tryGetValue<std::string>(currentValue).value_or(std::string{});
                int selectedIdx = 0;
                std::vector<const char*> items;
                items.reserve(entry.enumMapping.size());
                for (size_t i = 0; i < entry.enumMapping.size(); ++i) {
                    items.push_back(entry.enumMapping[i].second.c_str());
                    if (entry.enumMapping[i].second == strVal) selectedIdx = static_cast<int>(i);
                }
                if (!items.empty() && ImGui::Combo(label.c_str(), &selectedIdx, items.data(), static_cast<int>(items.size()))) {
                    if (selectedIdx >= 0 && selectedIdx < static_cast<int>(entry.enumMapping.size())) {
                        values.set<std::string>(entry.yamlPath, entry.enumMapping[selectedIdx].second);
                    }
                }
                break;
            }

            case Config::ConfigUiType::String: {
                auto strVal = Config::tryGetValue<std::string>(currentValue).value_or(std::string{});
                char buf[256]{};
                const size_t copyLen = std::min(strVal.size(), sizeof(buf) - 1);
                std::memcpy(buf, strVal.data(), copyLen);
                if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                    values.set<std::string>(entry.yamlPath, std::string(buf));
                }
                break;
            }
        }

        if (ImGui::IsItemHovered() && !entry.description.empty()) {
            ImGui::SetTooltip("%s\nPath: %s", entry.description.c_str(), entry.yamlPath.c_str());
        }
    }
}

void ConfigUIRenderer::RenderConfigStoreByModule(
    const Config::ConfigSchemaSnapshot& schema,
    Config::ConfigStore& values,
    const std::string& moduleTag) {

    Config::ConfigSchemaSnapshot filtered;
    for (const auto& entry : schema.entries) {
        if (entry.moduleTag == moduleTag) {
            filtered.entries.push_back(entry);
        }
    }

    RenderConfigStore(filtered, values, moduleTag);
}

std::vector<std::string> ConfigUIRenderer::CollectModuleTags(
    const Config::ConfigSchemaSnapshot& schema) {

    std::vector<std::string> tags;
    tags.reserve(schema.entries.size());
    for (const auto& entry : schema.entries) {
        if (!entry.moduleTag.empty()) {
            tags.push_back(entry.moduleTag);
        }
    }

    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

} // namespace App
