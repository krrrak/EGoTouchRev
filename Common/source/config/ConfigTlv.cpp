#include "config/ConfigTlv.h"

#include "Logger.h"

#include <cstring>
#include <limits>
#include <utility>

namespace Config {
namespace {

constexpr uint16_t kCurrentVersion = 1;
constexpr size_t kSnapshotHeaderSize = sizeof(uint16_t) + sizeof(uint16_t);
constexpr size_t kEntryHeaderSize = sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint16_t);
constexpr size_t kMutationResultSize = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint8_t);

void appendBytes(std::vector<uint8_t>& out, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

void appendUint16Le(std::vector<uint8_t>& out, uint16_t value)
{
    uint8_t bytes[sizeof(uint16_t)]{};
    std::memcpy(bytes, &value, sizeof(value));
    appendBytes(out, bytes, sizeof(bytes));
}

void appendUint8(std::vector<uint8_t>& out, uint8_t value)
{
    out.push_back(value);
}

bool readUint16Le(const uint8_t* data, size_t size, size_t& offset, uint16_t& value)
{
    if (offset > size || size - offset < sizeof(uint16_t)) {
        return false;
    }

    std::memcpy(&value, data + offset, sizeof(value));
    offset += sizeof(uint16_t);
    return true;
}

bool readUint8(const uint8_t* data, size_t size, size_t& offset, uint8_t& value)
{
    if (offset > size || size - offset < sizeof(uint8_t)) {
        return false;
    }

    value = data[offset];
    offset += sizeof(uint8_t);
    return true;
}

bool isKnownKeyId(ConfigKeyId keyId)
{
    return static_cast<uint16_t>(keyId) < static_cast<uint16_t>(ConfigKeyId::MaxKeyId);
}

bool isKnownValueType(ConfigValueType valueType)
{
    switch (valueType) {
    case ConfigValueType::Bool:
    case ConfigValueType::Int32:
    case ConfigValueType::Float:
    case ConfigValueType::String:
    case ConfigValueType::Null:
        return true;
    default:
        return false;
    }
}

void logUnknownKeyId(ConfigKeyId keyId)
{
    LOG_WARN("Config", "ConfigTlv", "Deserialize", "unknown keyId: 0x{:04X}", static_cast<uint16_t>(keyId));
}

void logMalformed(const char* method, const char* reason)
{
    LOG_WARN("Config", method, "Deserialize", "malformed TLV payload: {}", reason);
}

void logVersionMismatch(const char* method, uint16_t version)
{
    LOG_WARN("Config", method, "Deserialize", "unsupported TLV version: {}", version);
}

std::vector<uint8_t> serializeEntries(uint16_t version, const std::vector<ConfigTlvEntry>& entries)
{
    if (entries.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::length_error("too many Config TLV entries");
    }

    std::vector<uint8_t> out;
    out.reserve(kSnapshotHeaderSize + entries.size() * kEntryHeaderSize);
    appendUint16Le(out, version);
    appendUint16Le(out, static_cast<uint16_t>(entries.size()));

    for (const ConfigTlvEntry& entry : entries) {
        if (entry.stringValue.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::length_error("Config TLV entry payload is too large");
        }

        appendUint16Le(out, static_cast<uint16_t>(entry.keyId));
        appendUint8(out, static_cast<uint8_t>(entry.valueType));
        appendUint16Le(out, static_cast<uint16_t>(entry.stringValue.size()));
        appendBytes(out, entry.stringValue.data(), entry.stringValue.size());
    }

    return out;
}

template <typename T>
T deserializeEntries(const uint8_t* data, size_t size, const char* method)
{
    T result{};
    if (data == nullptr || size < kSnapshotHeaderSize) {
        logMalformed(method, "missing snapshot/patch header");
        return result;
    }

    size_t offset = 0;
    uint16_t version = 0;
    uint16_t entryCount = 0;
    if (!readUint16Le(data, size, offset, version) || !readUint16Le(data, size, offset, entryCount)) {
        logMalformed(method, "truncated snapshot/patch header");
        return T{};
    }

    if (version != kCurrentVersion) {
        logVersionMismatch(method, version);
        return T{};
    }

    result.version = version;
    result.entries.reserve(entryCount);

    for (uint16_t i = 0; i < entryCount; ++i) {
        uint16_t rawKeyId = 0;
        uint8_t rawValueType = 0;
        uint16_t valueLen = 0;
        if (!readUint16Le(data, size, offset, rawKeyId) ||
            !readUint8(data, size, offset, rawValueType) ||
            !readUint16Le(data, size, offset, valueLen)) {
            logMalformed(method, "truncated entry header");
            return T{};
        }

        if (offset > size || size - offset < valueLen) {
            logMalformed(method, "truncated entry payload");
            return T{};
        }

        std::string valuePayload(valueLen, '\0');
        if (valueLen > 0) {
            std::memcpy(valuePayload.data(), data + offset, valueLen);
        }
        offset += valueLen;

        ConfigTlvEntry entry{
            .keyId = static_cast<ConfigKeyId>(rawKeyId),
            .valueType = static_cast<ConfigValueType>(rawValueType),
            .stringValue = std::move(valuePayload),
        };

        if (!isKnownKeyId(entry.keyId)) {
            logUnknownKeyId(entry.keyId);
        }
        if (!isKnownValueType(entry.valueType)) {
            LOG_WARN("Config", method, "Deserialize", "unknown valueType: 0x{:02X}", rawValueType);
        }

        result.entries.push_back(std::move(entry));
    }

    if (offset != size) {
        LOG_WARN("Config", method, "Deserialize", "ignored {} trailing byte(s)", size - offset);
    }

    return result;
}

} // namespace

std::vector<uint8_t> serializeSnapshot(const ConfigSnapshotTlv& snapshot)
{
    return serializeEntries(snapshot.version, snapshot.entries);
}

ConfigSnapshotTlv deserializeSnapshot(const uint8_t* data, size_t size)
{
    return deserializeEntries<ConfigSnapshotTlv>(data, size, "deserializeSnapshot");
}

std::vector<uint8_t> serializePatch(const ConfigPatchTlv& patch)
{
    return serializeEntries(patch.version, patch.entries);
}

ConfigPatchTlv deserializePatch(const uint8_t* data, size_t size)
{
    return deserializeEntries<ConfigPatchTlv>(data, size, "deserializePatch");
}

std::vector<uint8_t> serializeMutationResult(const ConfigMutationResultTlv& result)
{
    std::vector<uint8_t> out;
    out.reserve(kMutationResultSize);
    appendUint16Le(out, result.changedCount);
    appendUint16Le(out, result.appliedCount);
    appendUint8(out, result.restartRequired ? uint8_t{1} : uint8_t{0});
    return out;
}

ConfigMutationResultTlv deserializeMutationResult(const uint8_t* data, size_t size)
{
    ConfigMutationResultTlv result{};
    if (data == nullptr || size < kMutationResultSize) {
        logMalformed("deserializeMutationResult", "missing mutation result payload");
        return result;
    }

    size_t offset = 0;
    uint8_t restartRequired = 0;
    if (!readUint16Le(data, size, offset, result.changedCount) ||
        !readUint16Le(data, size, offset, result.appliedCount) ||
        !readUint8(data, size, offset, restartRequired)) {
        logMalformed("deserializeMutationResult", "truncated mutation result payload");
        return ConfigMutationResultTlv{};
    }

    result.restartRequired = restartRequired != 0;

    if (offset != size) {
        LOG_WARN("Config", "deserializeMutationResult", "Deserialize", "ignored {} trailing byte(s)", size - offset);
    }

    return result;
}

} // namespace Config
