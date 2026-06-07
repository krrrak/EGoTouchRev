#include "ServiceProxyInternal.h"
#include "config/ConfigKeyMap.h"
#include "config/ConfigPath.h"
#include "config/ConfigStore.h"
#include "config/ConfigTlv.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

Config::ConfigV3CatalogPayload BuildCatalogPayload() {
    Config::ConfigV3CatalogPayload payload{};
    payload.schemaVersion = 10;
    payload.snapshotVersion = 20;
    const auto schema = App::BuildServiceProxyConfigSchemaSnapshotForTest();
    payload.entries.reserve(schema.entries.size());
    for (const auto& entry : schema.entries) {
        payload.entries.push_back(Config::ConfigDescriptor{
            .keyId = entry.keyId,
            .path = entry.yamlPath,
            .defaultValue = entry.defaultValue,
            .uiType = entry.uiType,
            .range = entry.range,
            .displayName = entry.displayName,
            .description = entry.description,
            .moduleTag = entry.moduleTag,
            .enumMapping = entry.enumMapping,
            .runtimeBinding = entry.runtimeBinding,
            .boundToRuntime = entry.boundToRuntime,
        });
    }
    return payload;
}

void ApplyCatalog(App::ServiceProxy& proxy) {
    const auto catalogBytes = Config::serializeConfigV3Catalog(BuildCatalogPayload());
    Require(proxy.ApplyConfigV3CatalogBytesForTest(catalogBytes.data(), catalogBytes.size()),
            "v3 catalog payload should apply");
}

void TestV3CatalogPayloadAppliesSchemaAndDefaults() {
    App::ServiceProxy proxy;
    ApplyCatalog(proxy);
    const auto& schema = proxy.GetConfigSchemaSnapshot();

    RequireLiveMappedEntry(schema, "service.mode");
    RequireLiveMappedEntry(schema, "touch.signal_cond.baseline_no_finger_max_step");
    RequireLiveMappedEntry(schema, "stylus.sp.bt_freq_shift_debounce_frames");
    Require(proxy.GetConfigStore().has("service.mode"), "local fallback store should remain populated");
    const auto versions = proxy.GetConfigV3BaselineVersionsForTest();
    Require(versions.catalogSchemaVersion == 10, "v3 catalog schemaVersion should be recorded");
    Require(versions.catalogSnapshotVersion == 20, "v3 catalog snapshotVersion should be recorded");
}

void TestLocalFallbackInitializesSchemaAndDefaultStore() {
    App::ServiceProxy proxy;
    Require(!proxy.GetConfigSchemaSnapshot().entries.empty(), "local fallback schema should initialize without Service connection");
    Require(proxy.GetConfigStore().has("service.mode"), "local fallback default store should contain service.mode");
    Require(proxy.GetConfigStore().has("touch.signal_cond.baseline_no_finger_max_step"),
            "local fallback default store should contain touch catalog key");
    const auto versions = proxy.GetConfigV3BaselineVersionsForTest();
    Require(versions.catalogSchemaVersion == 0, "local fallback should not claim a v3 catalog baseline");
    Require(versions.snapshotVersion == 0, "local fallback should not claim a v3 snapshot baseline");
}

void TestV3SnapshotPayloadWritesConfigStore() {
    App::ServiceProxy proxy;
    ApplyCatalog(proxy);

    Config::ConfigV3SnapshotPayload snapshot{};
    snapshot.schemaVersion = 10;
    snapshot.snapshotVersion = 21;
    snapshot.entries.push_back({Config::ConfigKeyId::SvcMode, int32_t{1}});
    snapshot.entries.push_back({Config::ConfigKeyId::SvcPenButtonRoute, std::string("win32_only")});
    snapshot.entries.push_back({Config::ConfigKeyId::SvcAutoMode, false});
    const auto bytes = Config::serializeConfigV3Snapshot(snapshot);

    Require(proxy.ApplyConfigV3SnapshotBytesForTest(bytes.data(), bytes.size()), "v3 snapshot payload should apply");
    Require(proxy.GetConfigStore().getOr<std::string>("service.mode", "") == "touch_only",
            "v3 snapshot should write numeric enum as string value to ConfigStore");
    Require(proxy.GetConfigStore().getOr<std::string>("service.pen_button_route", "") == "win32_only",
            "v3 snapshot should write string enum value to ConfigStore");
    Require(proxy.GetConfigStore().getOr<bool>("service.auto_mode", true) == false,
            "v3 snapshot should write bool value to ConfigStore");
    const auto versions = proxy.GetConfigV3BaselineVersionsForTest();
    Require(versions.snapshotSchemaVersion == 10, "v3 snapshot schemaVersion should be recorded");
    Require(versions.snapshotVersion == 21, "v3 snapshotVersion should be recorded");
}

void TestV3SnapshotPreservesDirtyPath() {
    App::ServiceProxy proxy;
    ApplyCatalog(proxy);
    proxy.GetConfigStore().set<Config::ConfigValue>("service.mode", std::string("full"));
    proxy.MarkConfigPathsDirty({"service.mode"});

    Config::ConfigV3SnapshotPayload snapshot{};
    snapshot.schemaVersion = 10;
    snapshot.snapshotVersion = 22;
    snapshot.entries.push_back({Config::ConfigKeyId::SvcMode, std::string("touch_only")});
    const auto bytes = Config::serializeConfigV3Snapshot(snapshot);

    Require(proxy.ApplyConfigV3SnapshotBytesForTest(bytes.data(), bytes.size()), "dirty v3 snapshot should still succeed");
    Require(proxy.GetConfigStore().getOr<std::string>("service.mode", "") == "full",
            "v3 snapshot should not overwrite dirty path");
}

void TestV3SnapshotSkipsUnknownKeyId() {
    App::ServiceProxy proxy;
    ApplyCatalog(proxy);
    proxy.GetConfigStore().set<Config::ConfigValue>("service.auto_mode", true);

    Config::ConfigV3SnapshotPayload snapshot{};
    snapshot.schemaVersion = 10;
    snapshot.snapshotVersion = 23;
    snapshot.entries.push_back({static_cast<Config::ConfigKeyId>(0x0200), int32_t{0}});
    snapshot.entries.push_back({Config::ConfigKeyId::SvcAutoMode, false});
    const auto bytes = Config::serializeConfigV3Snapshot(snapshot);

    Require(proxy.ApplyConfigV3SnapshotBytesForTest(bytes.data(), bytes.size()), "unknown keyId should be skipped safely");
    Require(proxy.GetConfigStore().getOr<bool>("service.auto_mode", true) == false,
            "known key after unknown keyId should still apply");
}

void TestV3SnapshotSkipsUnmappedNumericEnum() {
    App::ServiceProxy proxy;
    ApplyCatalog(proxy);
    proxy.GetConfigStore().set<Config::ConfigValue>("service.mode", std::string("full"));
    proxy.GetConfigStore().set<Config::ConfigValue>("service.auto_mode", true);

    Config::ConfigV3SnapshotPayload snapshot{};
    snapshot.schemaVersion = 10;
    snapshot.snapshotVersion = 24;
    snapshot.entries.push_back({Config::ConfigKeyId::SvcMode, int32_t{999}});
    snapshot.entries.push_back({Config::ConfigKeyId::SvcAutoMode, false});
    const auto bytes = Config::serializeConfigV3Snapshot(snapshot);

    Require(proxy.ApplyConfigV3SnapshotBytesForTest(bytes.data(), bytes.size()), "unmapped enum snapshot should not fail payload apply");
    Require(proxy.GetConfigStore().getOr<std::string>("service.mode", "") == "full",
            "unmapped numeric enum should not overwrite existing string enum value");
    Require(proxy.GetConfigStore().getOr<bool>("service.auto_mode", true) == false,
            "valid entry after unmapped numeric enum should still apply");
}

void TestInvalidV3PayloadDoesNotClearSchemaOrStore() {
    App::ServiceProxy proxy;
    const auto beforeEntries = proxy.GetConfigSchemaSnapshot().entries.size();
    proxy.GetConfigStore().set<Config::ConfigValue>("service.mode", std::string("full"));
    const std::vector<uint8_t> invalid{0, 1, 2, 3};

    Require(!proxy.ApplyConfigV3CatalogBytesForTest(invalid.data(), invalid.size()),
            "invalid v3 catalog should fail");
    Require(proxy.GetConfigSchemaSnapshot().entries.size() == beforeEntries,
            "failed v3 catalog should not clear schema");
    const auto beforeVersions = proxy.GetConfigV3BaselineVersionsForTest();
    Require(!proxy.ApplyConfigV3SnapshotBytesForTest(invalid.data(), invalid.size()),
            "invalid v3 snapshot should fail");
    Require(proxy.GetConfigStore().getOr<std::string>("service.mode", "") == "full",
            "failed v3 snapshot should not clear store");
    const auto afterVersions = proxy.GetConfigV3BaselineVersionsForTest();
    Require(afterVersions.snapshotSchemaVersion == beforeVersions.snapshotSchemaVersion,
            "failed v3 snapshot should not change recorded schemaVersion");
    Require(afterVersions.snapshotVersion == beforeVersions.snapshotVersion,
            "failed v3 snapshot should not change recorded snapshotVersion");
}

} // namespace

int main() {
    try {
        TestSchemaCoversServiceTouchAndStylusCatalogKeys();
        TestDirtyPathKeyIdsRemainStable();
        TestDefaultYamlContainsCatalogBackedKeys();
        TestV3CatalogPayloadAppliesSchemaAndDefaults();
        TestLocalFallbackInitializesSchemaAndDefaultStore();
        TestV3SnapshotPayloadWritesConfigStore();
        TestV3SnapshotPreservesDirtyPath();
        TestV3SnapshotSkipsUnknownKeyId();
        TestV3SnapshotSkipsUnmappedNumericEnum();
        TestInvalidV3PayloadDoesNotClearSchemaOrStore();
        std::cout << "[TEST] ServiceProxy catalog schema tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
