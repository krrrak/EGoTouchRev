#pragma once
#include "ConfigSchema.h"
#include "config/ConfigSchemaSnapshot.h"
#include <vector>
#include <string>
#include <optional>

namespace Config {
class ConfigStore;
}

namespace App {

// 将 ConfigParam 渲染为 ImGui 控件
class ConfigUIRenderer {
public:
    static void RenderConfigSchema(
        const std::vector<Solvers::ConfigParam>& schema,
        const std::string& sectionName,
        std::optional<Solvers::ConfigParam::Category> filterCategory = std::nullopt);

    /// Render only params whose moduleTag matches the given tag.
    static void RenderConfigSchemaByModule(
        const std::vector<Solvers::ConfigParam>& schema,
        const std::string& moduleTag);

    static void RenderConfigStore(
        const Config::ConfigSchemaSnapshot& schema,
        Config::ConfigStore& values,
        const std::string& sectionName);

    static void RenderConfigStoreByModule(
        const Config::ConfigSchemaSnapshot& schema,
        Config::ConfigStore& values,
        const std::string& moduleTag);

    static std::vector<std::string> CollectModuleTags(
        const Config::ConfigSchemaSnapshot& schema);
};

} // namespace App
