#pragma once
// ConfigSync: historical/legacy shared-memory config dirty signal.
// Current connected config uses Config v3 IPC; this flag only documents the
// old App config.ini write + Service reload compatibility background.

#include "Ipc/IpcSecurity.h"

#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Ipc {

constexpr const wchar_t* kConfigDirtyName = L"Global\\EGoTouchConfigDirty";

class ConfigDirtyFlag {
public:
    ConfigDirtyFlag() = default;
    ~ConfigDirtyFlag() { Close(); }
    ConfigDirtyFlag(const ConfigDirtyFlag&) = delete;
    ConfigDirtyFlag& operator=(const ConfigDirtyFlag&) = delete;

    // Open or create the shared flag
    bool Open() {
        ScopedSecurityDescriptor sd;
        SECURITY_ATTRIBUTES sa{};
        if (!BuildAdminOnlySecurityAttributes(sa, sd)) {
            return false;
        }

        m_mapHandle = OpenFileMappingW(
            FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kConfigDirtyName);
        if (!m_mapHandle && GetLastError() == ERROR_FILE_NOT_FOUND) {
            m_mapHandle = CreateFileMappingW(
                INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                0, sizeof(std::atomic<uint32_t>), kConfigDirtyName);
        }

        if (!m_mapHandle) return false;
        m_flag = static_cast<std::atomic<uint32_t>*>(
            MapViewOfFile(m_mapHandle, FILE_MAP_READ | FILE_MAP_WRITE,
                          0, 0, sizeof(std::atomic<uint32_t>)));
        if (!m_flag) {
            CloseHandle(m_mapHandle);
            m_mapHandle = nullptr;
            return false;
        }
        return true;
    }

    // Historical App-side dirty marker for legacy config.ini writes.
    void SetDirty() {
        if (m_flag) m_flag->store(1, std::memory_order_release);
    }

    // Historical Service-side poller for the legacy dirty marker.
    bool CheckAndClear() {
        if (!m_flag) return false;
        return m_flag->exchange(0, std::memory_order_acq_rel) != 0;
    }

    void Close() {
        if (m_flag) { UnmapViewOfFile(m_flag); m_flag = nullptr; }
        if (m_mapHandle) { CloseHandle(m_mapHandle); m_mapHandle = nullptr; }
    }

    bool IsOpen() const { return m_flag != nullptr; }

private:
    HANDLE m_mapHandle = nullptr;
    std::atomic<uint32_t>* m_flag = nullptr;
};

} // namespace Ipc
