#include "btmcu/PenUsbTransport.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto kPendingProofWindow = std::chrono::milliseconds(150);
constexpr auto kReaderExitTimeout = std::chrono::seconds(2);
constexpr uint32_t kLongReadTimeoutMs = 30000;

struct HandleGuard {
    HANDLE value = INVALID_HANDLE_VALUE;

    ~HandleGuard() { Reset(); }

    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) : value(handle) {}

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
        value = handle;
    }

    bool IsValid() const noexcept {
        return value != INVALID_HANDLE_VALUE && value != nullptr;
    }
};

struct PipeFixture {
    std::wstring path;
    HandleGuard server;

    explicit PipeFixture(const wchar_t* scenario) {
        std::wostringstream stream;
        stream << L"\\\\.\\pipe\\EGoTouchPenUsbTransportLifecycle_"
               << GetCurrentProcessId() << L"_" << GetCurrentThreadId()
               << L"_" << scenario;
        path = stream.str();
    }

    bool CreateServer() {
        server.Reset(CreateNamedPipeW(path.c_str(),
                                      PIPE_ACCESS_DUPLEX,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                      1,
                                      64,
                                      64,
                                      0,
                                      nullptr));
        if (!server.IsValid()) {
            std::wcerr << L"[TEST] CreateNamedPipeW failed for " << path
                       << L" error=" << GetLastError() << L"\n";
            return false;
        }
        return true;
    }

    bool OpenClientAndConnect(Himax::Pen::IPenUsbTransport& transport) {
        auto open_result = transport.Open(path);
        if (!open_result) {
            std::wcerr << L"[TEST] transport.Open failed for " << path << L"\n";
            return false;
        }

        if (!ConnectNamedPipe(server.value, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) {
                std::wcerr << L"[TEST] ConnectNamedPipe failed for " << path
                           << L" error=" << error << L"\n";
                return false;
            }
        }
        return true;
    }
};

class AsyncReadProbe {
public:
    explicit AsyncReadProbe(Himax::Pen::IPenUsbTransport& transport)
        : transport_(transport) {}

    ~AsyncReadProbe() {
        if (thread_.joinable()) {
            thread_.detach();
        }
    }

    void Start() {
        thread_ = std::thread([this]() {
            Himax::Pen::PenUsbPacketBuffer bytes;
            result_ok_.store(transport_.ReadPacket(bytes, kLongReadTimeoutMs).has_value(),
                             std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mu_);
                done_ = true;
            }
            cv_.notify_all();
        });
    }

    bool ProveStillPending() {
        std::unique_lock<std::mutex> lock(mu_);
        return !cv_.wait_for(lock, kPendingProofWindow, [this]() { return done_; });
    }

    bool WaitForFailedReturn() {
        {
            std::unique_lock<std::mutex> lock(mu_);
            if (!cv_.wait_for(lock, kReaderExitTimeout, [this]() { return done_; })) {
                return false;
            }
        }

        if (thread_.joinable()) {
            thread_.join();
        }
        return !result_ok_.load(std::memory_order_acquire);
    }

private:
    Himax::Pen::IPenUsbTransport& transport_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic<bool> result_ok_{true};
};

bool RunPendingReadCloseTest() {
    PipeFixture pipe(L"Close");
    if (!pipe.CreateServer()) {
        return false;
    }

    auto transport = Himax::Pen::CreatePenUsbTransportWin32();
    if (!pipe.OpenClientAndConnect(*transport)) {
        return false;
    }

    AsyncReadProbe reader(*transport);
    reader.Start();
    if (!reader.ProveStillPending()) {
        transport->Close();
        std::cerr << "[TEST] ReadPacket returned before Close while server kept pipe idle.\n";
        return false;
    }

    transport->Close();
    if (!reader.WaitForFailedReturn()) {
        std::cerr << "[TEST] Pending ReadPacket did not fail promptly after Close.\n";
        return false;
    }
    return true;
}

bool RunPendingReadCancelThenCloseTest() {
    PipeFixture pipe(L"CancelThenClose");
    if (!pipe.CreateServer()) {
        return false;
    }

    auto transport = Himax::Pen::CreatePenUsbTransportWin32();
    if (!pipe.OpenClientAndConnect(*transport)) {
        return false;
    }

    AsyncReadProbe reader(*transport);
    reader.Start();
    if (!reader.ProveStillPending()) {
        transport->Close();
        std::cerr << "[TEST] ReadPacket returned before CancelIo while server kept pipe idle.\n";
        return false;
    }

    transport->CancelIo();
    transport->Close();
    if (!reader.WaitForFailedReturn()) {
        std::cerr << "[TEST] Pending ReadPacket did not fail promptly after CancelIo then Close.\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!RunPendingReadCloseTest()) {
        return 1;
    }

    if (!RunPendingReadCancelThenCloseTest()) {
        return 2;
    }

    std::cout << "[TEST] PenUsbTransportWin32 lifecycle tests passed.\n";
    return 0;
}
