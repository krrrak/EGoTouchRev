#include "ConfigRuntime.h"

#include "config/ConfigCatalog.h"
#include "config/ConfigStore.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#ifndef EGO_REPO_DEFAULT_CONFIG_PATH
#error EGO_REPO_DEFAULT_CONFIG_PATH must point to config/default.yaml
#endif

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string ValueDebugString(const Config::ConfigValue& value)
{
    return std::visit([](const auto& typedValue) -> std::string {
        using ValueType = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<ValueType, bool>) {
            return typedValue ? "bool:true" : "bool:false";
        } else if constexpr (std::is_same_v<ValueType, std::string>) {
            return "string:" + typedValue;
        } else if constexpr (std::is_same_v<ValueType, float>) {
            return "float:" + std::to_string(typedValue);
        } else {
            return "int32:" + std::to_string(typedValue);
        }
    }, value);
}

bool EquivalentValue(const Config::ConfigValue& lhs, const Config::ConfigValue& rhs)
{
    if (lhs.index() == rhs.index()) {
        return lhs == rhs;
    }

    const auto lhsInt = Config::tryGetValue<int32_t>(lhs);
    const auto rhsInt = Config::tryGetValue<int32_t>(rhs);
    const auto lhsFloat = Config::tryGetValue<float>(lhs);
    const auto rhsFloat = Config::tryGetValue<float>(rhs);
    if ((lhsInt.has_value() || lhsFloat.has_value()) && (rhsInt.has_value() || rhsFloat.has_value())) {
        const double lhsNumber = lhsInt.has_value() ? static_cast<double>(*lhsInt) : static_cast<double>(*lhsFloat);
        const double rhsNumber = rhsInt.has_value() ? static_cast<double>(*rhsInt) : static_cast<double>(*rhsFloat);
        return std::fabs(lhsNumber - rhsNumber) < 0.000001;
    }

    return false;
}

bool IsExpectedYamlOnlyPath(std::string_view)
{
    // Intentional YAML-only keys must be listed here with a reason. The P1-5
    // baseline has no exceptions; extra repository keys should fail the test.
    return false;
}

std::set<std::string> CatalogDefaultPaths(const Config::ConfigSchemaSnapshot& schema)
{
    std::set<std::string> paths;
    const auto catalog = Config::ConfigCatalogBuilder::fromSnapshot(schema);
    for (const auto& descriptor : catalog.descriptors()) {
        paths.insert(descriptor.path);
    }
    return paths;
}

} // namespace

int main()
{
    try {
        const auto generatedDefaults = Service::ConfigRuntime::BuildFactoryDefaultStore();
        const auto generatedSchema = Service::ConfigRuntime::BuildFactoryDefaultSchema();
        const auto catalogPaths = CatalogDefaultPaths(generatedSchema);

        const auto outputDir = std::filesystem::current_path() / "generated-config";
        std::filesystem::create_directories(outputDir);
        const auto outputPath = outputDir / "default.yaml";
        generatedDefaults.saveToYaml(outputPath.string());
        Config::ConfigStore generatedRoundtrip;
        generatedRoundtrip.loadFromYaml(outputPath.string());

        Config::ConfigStore repoDefaults;
        repoDefaults.loadFromYaml(EGO_REPO_DEFAULT_CONFIG_PATH);

        for (const auto& path : catalogPaths) {
            Require(generatedDefaults.has(path), "catalog default path is missing from generated defaults: " + path);
            Require(generatedRoundtrip.has(path), "generated YAML roundtrip is missing catalog path: " + path);
            Require(repoDefaults.has(path), "config/default.yaml is missing catalog default path: " + path);

            const auto generatedValue = generatedDefaults.get<Config::ConfigValue>(path);
            const auto generatedYamlValue = generatedRoundtrip.get<Config::ConfigValue>(path);
            const auto repoValue = repoDefaults.get<Config::ConfigValue>(path);
            Require(EquivalentValue(generatedValue, generatedYamlValue),
                    "generated YAML changed default value for " + path + ": " +
                    ValueDebugString(generatedValue) + " vs " + ValueDebugString(generatedYamlValue));
            Require(EquivalentValue(generatedValue, repoValue),
                    "config/default.yaml drift at " + path + ": generated " +
                    ValueDebugString(generatedValue) + ", repo " + ValueDebugString(repoValue));
        }

        for (const auto& path : repoDefaults.allPaths()) {
            Require(catalogPaths.contains(path) || IsExpectedYamlOnlyPath(path),
                    "config/default.yaml has YAML-only key not registered in catalog defaults: " + path);
        }

        std::cout << "[TEST] ConfigDefaultYamlDriftTest passed. Generated YAML: "
                  << outputPath.string() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] ConfigDefaultYamlDriftTest failed: " << ex.what() << '\n';
        return 1;
    }
}
