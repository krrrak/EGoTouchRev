#include "ServiceProxyInternal.h"
#include "config/ConfigKeyMap.h"
#include "config/ConfigPath.h"
#include "config/ConfigStore.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const Config::ConfigSchemaEntry* FindEntry(const Config::ConfigSchemaSnapshot& schema,
                                           std::string_view path) {
    const auto it = std::ranges::find_if(schema.entries, [path](const Config::ConfigSchemaEntry& entry) {
        return entry.yamlPath == path;
    });
    return it == schema.entries.end() ? nullptr : &*it;
}

void RequireLiveMappedEntry(const Config::ConfigSchemaSnapshot& schema,
                            std::string_view path) {
    const auto* entry = FindEntry(schema, path);
    Require(entry != nullptr, "schema entry should exist");
    Require(entry->boundToRuntime, "schema entry should be bound to runtime");
    Require(entry->runtimeBinding == Config::ConfigRuntimeBinding::ManualLiveApply ||
            entry->runtimeBinding == Config::ConfigRuntimeBinding::LiveSetter,
            "schema entry should be live patchable");

    const auto catalogKeyId = Config::tryKeyIdForPath(path);
    Require(catalogKeyId.has_value(), "catalog keyId should exist for schema path");
    Require(entry->keyId == *catalogKeyId, "schema keyId should match shared key map");
}

void TestSchemaCoversServiceTouchAndStylusCatalogKeys() {
    const auto schema = App::BuildServiceProxyConfigSchemaSnapshotForTest();

    RequireLiveMappedEntry(schema, "service.mode");
    RequireLiveMappedEntry(schema, "service.auto_mode");
    RequireLiveMappedEntry(schema, "service.stylus_vhf_enabled");
    RequireLiveMappedEntry(schema, "service.pen_button_mode");
    RequireLiveMappedEntry(schema, "service.pen_button_route");
    RequireLiveMappedEntry(schema, "touch.signal_cond.baseline_no_finger_max_step");
    RequireLiveMappedEntry(schema, "stylus.sp.bt_freq_shift_debounce_frames");
    RequireLiveMappedEntry(schema, "stylus.sp.lock_flash_edge_y");
}

void TestDirtyPathKeyIdsRemainStable() {
    const std::pair<std::string_view, Config::ConfigKeyId> expected[] = {
        {"service.mode", Config::ConfigKeyId::SvcMode},
        {"service.auto_mode", Config::ConfigKeyId::SvcAutoMode},
        {"service.stylus_vhf_enabled", Config::ConfigKeyId::SvcStylusVhfEnabled},
        {"service.pen_button_mode", Config::ConfigKeyId::SvcPenButtonMode},
        {"service.pen_button_route", Config::ConfigKeyId::SvcPenButtonRoute},
        {"touch.signal_cond.baseline_no_finger_max_step", Config::ConfigKeyId::TouchBaselineNoFingerMaxStep},
        {"stylus.sp.bt_freq_shift_debounce_frames", Config::ConfigKeyId::StylusSpBtFreqShiftDebounceFrames},
    };

    for (const auto& [path, keyId] : expected) {
        const auto resolved = Config::tryKeyIdForPath(path);
        Require(resolved.has_value(), "dirty path should resolve to shared keyId");
        Require(*resolved == keyId, "dirty path keyId should remain stable");
    }
}

void TestDefaultYamlContainsCatalogBackedKeys() {
    const auto paths = Config::resolve();
    Require(paths.has_value(), "config/default.yaml should be resolvable");

    Config::ConfigStore store;
    store.loadFromYaml(paths->defaultConfig);

    Require(store.has("service.mode"), "default.yaml should contain service.mode");
    Require(store.has("service.pen_button_route"), "default.yaml should contain service.pen_button_route");
    Require(store.has("touch.signal_cond.baseline_no_finger_max_step"), "default.yaml should contain touch baseline key");
    Require(store.has("stylus.sp.bt_freq_shift_debounce_frames"), "default.yaml should contain stylus SP key");
    Require(store.has("stylus.sp.lock_flash_edge_y"), "default.yaml should contain stylus lock key");
}

} // namespace

int main() {
    try {
        TestSchemaCoversServiceTouchAndStylusCatalogKeys();
        TestDirtyPathKeyIdsRemainStable();
        TestDefaultYamlContainsCatalogBackedKeys();
        std::cout << "[TEST] ServiceProxy catalog schema tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
