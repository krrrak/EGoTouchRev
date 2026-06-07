#include "ServiceProxyInternal.h"

namespace App {

ServiceProxy::ServiceProxy()
    : m_dvrBuffer(std::make_unique<RingBuffer<Dvr::DvrFrameSlot, kDvrCapacity>>()),
      m_dvrDynamicDebugBuffer(std::make_unique<RingBuffer<Dvr::DvrDynamicDebugFrameSlot, kDvrCapacity>>()) {
    InitConfigSchema();
    RefreshConfigSnapshot();
}

ServiceProxy::~ServiceProxy() = default;

bool ServiceProxy::Connect() {
    return false;
}

void ServiceProxy::Disconnect() {}

bool ServiceProxy::TryConnect() {
    return Connect();
}

bool ServiceProxy::IsConnected() const {
    return false;
}

} // namespace App
