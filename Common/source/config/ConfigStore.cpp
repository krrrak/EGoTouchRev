#include "config/ConfigStore.h"

#include <algorithm>
#include <optional>

namespace Config {
namespace {

std::optional<double> numericValue(const ConfigValue& value) {
    return std::visit([](const auto& typedValue) -> std::optional<double> {
        using ValueType = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<ValueType, int32_t> || std::is_same_v<ValueType, float>) {
            return static_cast<double>(typedValue);
        } else {
            return std::nullopt;
        }
    }, value);
}

} // namespace

ValidationResult ConfigStore::validate() const {
    ValidationResult result;

    for (const auto& [path, entry] : m_entries) {
        if (entry.defaultValue.has_value() && entry.value.index() != entry.defaultValue->index()) {
            result.errors.push_back({
                ValidationIssue::Error,
                path,
                "type mismatch with default value"
            });
            continue;
        }

        if (entry.range.has_value()) {
            const auto value = numericValue(entry.value);
            if (!value.has_value()) {
                result.errors.push_back({
                    ValidationIssue::Error,
                    path,
                    "range validation requires a numeric value"
                });
                continue;
            }

            if (*value < entry.range->min || *value > entry.range->max) {
                result.errors.push_back({
                    ValidationIssue::Error,
                    path,
                    "value is outside configured range"
                });
            }
        }
    }

    return result;
}

std::vector<std::string> ConfigStore::allPaths() const {
    std::vector<std::string> paths;
    paths.reserve(m_entries.size());
    for (const auto& [key, _] : m_entries) {
        paths.push_back(key);
    }
    std::ranges::sort(paths);
    return paths;
}

bool ConfigStore::has(std::string_view path) const {
    const auto key = resolvePath(path);
    return m_entries.contains(key);
}

void ConfigStore::mergeFrom(const ConfigStore& other) {
    for (const auto& [key, entry] : other.m_entries) {
        m_entries[key] = entry;
    }
}

std::string ConfigStore::resolvePath(std::string_view path) const {
    return std::string(path);
}

} // namespace Config
