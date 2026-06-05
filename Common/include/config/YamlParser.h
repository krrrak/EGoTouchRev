#pragma once

#include "ConfigValue.h"
#include <string>
#include <yaml-cpp/yaml.h>

namespace Config {

class YamlParser {
public:
    // 从文件加载 YAML，返回根节点
    static YAML::Node load(const std::string& filePath);

    // 将 YAML 节点保存到文件
    static void save(const std::string& filePath, const YAML::Node& node);

    // 合并两个 YAML 节点（overrides 覆盖 default 的同名键，递归）
    static YAML::Node merge(const YAML::Node& base, const YAML::Node& overlay);
};

} // namespace Config
