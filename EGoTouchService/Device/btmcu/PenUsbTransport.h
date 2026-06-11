#pragma once

#include "btmcu/PenUsbTypes.h"
#include "common/DeviceError.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace Himax::Pen {

class IPenUsbTransport {
public:
    virtual ~IPenUsbTransport() = default;

    virtual ChipResult<> Open(const std::wstring& devicePath) = 0;
    virtual void Close() = 0;
    virtual void CancelIo() = 0;
    virtual bool IsOpen() const = 0;

    virtual ChipResult<> ReadPacket(PenUsbPacketBuffer& outPacket, uint32_t timeoutMs) = 0;
    virtual ChipResult<> WritePacket(std::span<const uint8_t> bytes) = 0;
};

std::unique_ptr<IPenUsbTransport> CreatePenUsbTransportWin32();

} // namespace Himax::Pen
