#include "btmcu/PenUsbTransport.h"
#include <array>
#include <mutex>
#include <windows.h>

namespace Himax::Pen {

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

        m_readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_readEvent || !m_writeEvent) {
            CloseLocked();
            return std::unexpected(ChipError::CommunicationError);
        }

        m_readOv = {};
        m_readOv.hEvent = m_readEvent;
        m_readPending = false;
        return {};
    }

    void Close() override {
        std::lock_guard<std::mutex> lk(m_handleMu);
        CloseLocked();
    }

    void CancelIo() override {
        std::lock_guard<std::mutex> lk(m_handleMu);
        if (m_handle != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_handle, nullptr);
        }
    }

    bool IsOpen() const override {
        std::lock_guard<std::mutex> lk(m_handleMu);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    ChipResult<> ReadPacket(std::vector<uint8_t>& outBytes, uint32_t timeoutMs) override {
        HANDLE handle = INVALID_HANDLE_VALUE;
        HANDLE readEvent = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_handleMu);
            if (m_handle == INVALID_HANDLE_VALUE || !m_readEvent) {
                return std::unexpected(ChipError::InvalidOperation);
            }
            handle = m_handle;
            readEvent = m_readEvent;
            if (m_readPending) {
                DWORD drainedBytes = 0;
                if (!TryDrainReadLocked(drainedBytes)) {
                    return std::unexpected(ChipError::Timeout);
                }
            }

            m_readBuffer = {};
            m_readBytes = 0;
            m_readOv = {};
            m_readOv.hEvent = m_readEvent;
            ::ResetEvent(m_readEvent);
        }

        BOOL ok = ::ReadFile(handle,
                             m_readBuffer.data(),
                             static_cast<DWORD>(m_readBuffer.size()),
                             &m_readBytes,
                             &m_readOv);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }
            {
                std::lock_guard<std::mutex> lk(m_handleMu);
                m_readPending = true;
            }

            DWORD waitRes = ::WaitForSingleObject(readEvent, timeoutMs);
            if (waitRes == WAIT_TIMEOUT) {
                std::lock_guard<std::mutex> lk(m_handleMu);
                if (m_handle != INVALID_HANDLE_VALUE) {
                    ::CancelIoEx(m_handle, &m_readOv);
                }
                (void)DrainReadLocked(50);
                return std::unexpected(ChipError::Timeout);
            } else if (waitRes != WAIT_OBJECT_0) {
                std::lock_guard<std::mutex> lk(m_handleMu);
                if (m_handle != INVALID_HANDLE_VALUE) {
                    ::CancelIoEx(m_handle, &m_readOv);
                }
                (void)DrainReadLocked(50);
                return std::unexpected(ChipError::CommunicationError);
            }

            std::lock_guard<std::mutex> lk(m_handleMu);
            if (!TryDrainReadLocked(m_readBytes)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (m_readBytes == 0) {
            return std::unexpected(ChipError::Timeout);
        }

        outBytes.assign(m_readBuffer.begin(), m_readBuffer.begin() + static_cast<std::ptrdiff_t>(m_readBytes));
        return {};
    }

    ChipResult<> WritePacket(std::span<const uint8_t> bytes) override {
        HANDLE handle = INVALID_HANDLE_VALUE;
        HANDLE writeEvent = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_handleMu);
            if (m_handle == INVALID_HANDLE_VALUE || !m_writeEvent) {
                return std::unexpected(ChipError::InvalidOperation);
            }
            handle = m_handle;
            writeEvent = m_writeEvent;
            ::ResetEvent(m_writeEvent);
        }
        if (bytes.empty()) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        DWORD bytesWritten = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = writeEvent;

        BOOL ok = ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, &overlapped);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }

            DWORD waitRes = ::WaitForSingleObject(writeEvent, 2000);
            if (waitRes == WAIT_TIMEOUT) {
                ::CancelIoEx(handle, &overlapped);
                ::GetOverlappedResult(handle, &overlapped, &bytesWritten, TRUE);
                return std::unexpected(ChipError::Timeout);
            } else if (waitRes != WAIT_OBJECT_0) {
                ::CancelIoEx(handle, &overlapped);
                ::GetOverlappedResult(handle, &overlapped, &bytesWritten, TRUE);
                return std::unexpected(ChipError::CommunicationError);
            }

            if (!::GetOverlappedResult(handle, &overlapped, &bytesWritten, FALSE)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (bytesWritten != bytes.size()) {
            return std::unexpected(ChipError::CommunicationError);
        }
        return {};
    }

private:
    bool TryDrainReadLocked(DWORD& bytesTransferred) {
        if (m_handle == INVALID_HANDLE_VALUE || !m_readPending) {
            m_readPending = false;
            return true;
        }

        if (!::GetOverlappedResult(m_handle, &m_readOv, &bytesTransferred, FALSE)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_INCOMPLETE) {
                return false;
            }
        }
        m_readPending = false;
        return true;
    }

    bool DrainReadLocked(DWORD waitMs) {
        if (!m_readPending || !m_readEvent) {
            return true;
        }
        if (::WaitForSingleObject(m_readEvent, waitMs) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD ignored = 0;
        return TryDrainReadLocked(ignored);
    }

    void CloseLocked() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_handle, nullptr);
            if (m_readPending && !DrainReadLocked(INFINITE)) {
                // Preserve the handle, event, OVERLAPPED storage and pending flag
                // rather than freeing resources that the kernel may still signal.
                return;
            }
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
        if (m_readEvent) {
            ::CloseHandle(m_readEvent);
            m_readEvent = nullptr;
        }
        if (m_writeEvent) {
            ::CloseHandle(m_writeEvent);
            m_writeEvent = nullptr;
        }
        m_readPending = false;
    }

    mutable std::mutex m_handleMu;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    HANDLE m_readEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
    OVERLAPPED m_readOv{};
    std::array<uint8_t, 64> m_readBuffer{};
    DWORD m_readBytes = 0;
    bool m_readPending = false;
};

std::unique_ptr<IPenUsbTransport> CreatePenUsbTransportWin32() {
    return std::make_unique<PenUsbTransportWin32>();
}

} // namespace Himax::Pen
