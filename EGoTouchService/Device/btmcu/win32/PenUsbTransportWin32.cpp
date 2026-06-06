#include "btmcu/PenUsbTransport.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>
#include <windows.h>

namespace Himax::Pen {

namespace {

struct EventHandle {
    HANDLE value = nullptr;

    EventHandle() = default;
    explicit EventHandle(HANDLE h) : value(h) {}
    ~EventHandle() { Reset(); }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    EventHandle(EventHandle&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }

    EventHandle& operator=(EventHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    void Reset(HANDLE h = nullptr) noexcept {
        if (value) {
            ::CloseHandle(value);
        }
        value = h;
    }

    bool IsValid() const noexcept { return value != nullptr; }
};

} // namespace

class PenUsbTransportWin32 final : public IPenUsbTransport {
public:
    PenUsbTransportWin32() = default;
    ~PenUsbTransportWin32() override { Close(); }

    ChipResult<> Open(const std::wstring& devicePath) override {
        Close();

        std::lock_guard<std::mutex> lk(m_handleMu);
        m_handle = ::CreateFileW(devicePath.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                 nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(ChipError::CommunicationError);
        }

        m_cancelEvent.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!m_cancelEvent.IsValid()) {
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
            return std::unexpected(ChipError::CommunicationError);
        }

        m_closing = false;
        return {};
    }

    void Close() override {
        std::unique_lock<std::mutex> lk(m_handleMu);
        if (m_handle == INVALID_HANDLE_VALUE) {
            m_closing = false;
            return;
        }

        m_closing = true;
        const HANDLE handle = m_handle;
        if (m_cancelEvent.IsValid()) {
            ::SetEvent(m_cancelEvent.value);
        }
        ::CancelIoEx(handle, nullptr);
        m_idleCv.wait(lk, [this]() { return m_activeIo == 0; });

        ::CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        m_cancelEvent.Reset();
        m_closing = false;
    }

    void CancelIo() override {
        std::lock_guard<std::mutex> lk(m_handleMu);
        if (m_cancelEvent.IsValid()) {
            ::SetEvent(m_cancelEvent.value);
        }
        if (m_handle != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_handle, nullptr);
        }
    }

    bool IsOpen() const override {
        std::lock_guard<std::mutex> lk(m_handleMu);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    ChipResult<> ReadPacket(std::vector<uint8_t>& outBytes, uint32_t timeoutMs) override {
        EventHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.IsValid()) {
            return std::unexpected(ChipError::CommunicationError);
        }

        OVERLAPPED ov{};
        ov.hEvent = event.value;
        std::array<uint8_t, 64> buffer{};
        DWORD bytesRead = 0;
        IoLease lease(*this);
        if (!lease.IsValid()) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        BOOL ok = ::ReadFile(lease.Handle(),
                             buffer.data(),
                             static_cast<DWORD>(buffer.size()),
                             &bytesRead,
                             &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }

            const HANDLE waitHandles[] = {event.value, lease.CancelEvent()};
            const DWORD waitRes = ::WaitForMultipleObjects(2, waitHandles, FALSE, timeoutMs);
            if (waitRes == WAIT_TIMEOUT) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::Timeout);
            }
            if (waitRes == WAIT_OBJECT_0 + 1) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::CommunicationError);
            }
            if (waitRes != WAIT_OBJECT_0) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::CommunicationError);
            }
            if (!::GetOverlappedResult(lease.Handle(), &ov, &bytesRead, FALSE)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (bytesRead == 0) {
            return std::unexpected(ChipError::Timeout);
        }

        outBytes.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead));
        return {};
    }

    ChipResult<> WritePacket(std::span<const uint8_t> bytes) override {
        if (bytes.empty() || bytes.size() > 64) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        EventHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.IsValid()) {
            return std::unexpected(ChipError::CommunicationError);
        }

        OVERLAPPED ov{};
        ov.hEvent = event.value;
        std::array<uint8_t, 64> buffer{};
        std::copy(bytes.begin(), bytes.end(), buffer.begin());
        DWORD bytesWritten = 0;
        IoLease lease(*this);
        if (!lease.IsValid()) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        BOOL ok = ::WriteFile(lease.Handle(),
                              buffer.data(),
                              static_cast<DWORD>(bytes.size()),
                              &bytesWritten,
                              &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }

            const HANDLE waitHandles[] = {event.value, lease.CancelEvent()};
            const DWORD waitRes = ::WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
            if (waitRes == WAIT_TIMEOUT) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::Timeout);
            }
            if (waitRes == WAIT_OBJECT_0 + 1) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::CommunicationError);
            }
            if (waitRes != WAIT_OBJECT_0) {
                ::CancelIoEx(lease.Handle(), &ov);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(lease.Handle(), &ov, &ignored, TRUE);
                return std::unexpected(ChipError::CommunicationError);
            }
            if (!::GetOverlappedResult(lease.Handle(), &ov, &bytesWritten, FALSE)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (bytesWritten != bytes.size()) {
            return std::unexpected(ChipError::CommunicationError);
        }
        return {};
    }

private:
    class IoLease {
    public:
        explicit IoLease(PenUsbTransportWin32& owner) : m_owner(&owner) {
            std::lock_guard<std::mutex> lk(owner.m_handleMu);
            if (owner.m_handle == INVALID_HANDLE_VALUE || owner.m_closing) {
                return;
            }
            if (owner.m_activeIo == 0 && owner.m_cancelEvent.IsValid()) {
                ::ResetEvent(owner.m_cancelEvent.value);
            }
            m_handle = owner.m_handle;
            m_cancelEvent = owner.m_cancelEvent.value;
            ++owner.m_activeIo;
            m_valid = true;
        }

        ~IoLease() {
            if (!m_valid || !m_owner) {
                return;
            }
            std::lock_guard<std::mutex> lk(m_owner->m_handleMu);
            if (m_owner->m_activeIo > 0) {
                --m_owner->m_activeIo;
            }
            m_owner->m_idleCv.notify_all();
        }

        IoLease(const IoLease&) = delete;
        IoLease& operator=(const IoLease&) = delete;

        bool IsValid() const noexcept { return m_valid; }
        HANDLE Handle() const noexcept { return m_handle; }
        HANDLE CancelEvent() const noexcept { return m_cancelEvent; }

    private:
        PenUsbTransportWin32* m_owner = nullptr;
        HANDLE m_handle = INVALID_HANDLE_VALUE;
        HANDLE m_cancelEvent = nullptr;
        bool m_valid = false;
    };

    mutable std::mutex m_handleMu;
    std::condition_variable m_idleCv;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    EventHandle m_cancelEvent;
    uint32_t m_activeIo = 0;
    bool m_closing = false;
};

std::unique_ptr<IPenUsbTransport> CreatePenUsbTransportWin32() {
    return std::make_unique<PenUsbTransportWin32>();
}

} // namespace Himax::Pen
