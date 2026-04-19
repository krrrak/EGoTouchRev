#pragma once

#include "SystemStateMonitor.h"
#include "runtime/DeviceRuntime.h"
#include "penevt/PenEventBridge.h"
#include "penpress/PenPressureReader.h"
#include "IpcPipeServer.h"
#include "SharedFrameBuffer.h"
#include "ConfigSync.h"
#include "IpcProtocol.h"
#include "SolverTypes.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Service {

/// 服务运行模式
enum class ServiceMode {
    Full,       ///< 触摸 + 手写笔（PenEventBridge + PenPressureReader + StylusPipeline 全部启用）
    TouchOnly,  ///< 仅触摸（跳过手写笔模块 / StylusPipeline）
};

/// 模块加载器：负责创建、连接、启停所有子模块。
/// 不知道 SCM 的存在，可以独立测试。
class ServiceHost {
public:
    ServiceHost() = default;
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    /// 按依赖顺序启动所有模块
    bool Start();

    /// 逆序停止所有模块
    void Stop();

    ServiceMode GetMode() const { return m_mode; }

private:
    ServiceMode m_mode = ServiceMode::TouchOnly;
    bool m_autoMode = true;
    bool m_stylusVhfEnabled = true;

    std::unique_ptr<DeviceRuntime>                   m_deviceRuntime;
    std::unique_ptr<Host::SystemStateMonitor>         m_sysMonitor;
    // BT MCU 事件通道 (col00)：设备发现 + 握手 + ACK + 0x7D01 回显 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenEventBridge>       m_penEventBridge;
    // BT MCU 压力通道 (col01)：'U' 报文读取 + 频率 / 压感数据 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenPressureReader>    m_penPressureReader;

    // IPC
    Ipc::IpcPipeServer      m_ipcServer;
    Ipc::SharedFrameWriter  m_frameWriter;
    Ipc::ConfigDirtyFlag    m_configDirty;
    bool                    m_debugMode = false;
    HANDLE                  m_logEvent = nullptr;
    HANDLE                  m_penEvent = nullptr;

    struct DebugFieldDef {
        uint16_t fieldId = 0;
        Ipc::DebugValueType valueType = Ipc::DebugValueType::UInt32;
        Ipc::DebugSourceKind sourceKind = Ipc::DebugSourceKind::DerivedField;
        int16_t sourceIndex = -1;
        uint8_t uiOrder = 0;
        Ipc::DebugDvrTarget dvrTarget = Ipc::DebugDvrTarget::None;
        Ipc::DebugDvrPositionMode dvrPositionMode = Ipc::DebugDvrPositionMode::Append;
        int16_t dvrIndex = -1;
        std::string key;
        std::string displayName;
        std::string unit;
        std::string uiGroup;
        std::string dvrColumnName;
        std::string dvrAnchor;
    };

    std::vector<DebugFieldDef> m_debugSchema;
    uint16_t m_debugSchemaVersion = 1;
    uint32_t m_debugSchemaHash = 0;
    mutable std::mutex m_debugFrameMutex;
    Solvers::HeatmapFrame m_latestDebugFrame;
    bool m_hasLatestDebugFrame = false;

    static void CopyCString(char* dst, size_t dstSize, const std::string& src);
    static uint32_t HashDebugSchema(const std::vector<DebugFieldDef>& defs);
    static uint64_t EncodeDebugValue(const Solvers::HeatmapFrame& frame,
                                     const DebugFieldDef& def,
                                     bool& valid);
    static uint64_t EncodePenValue(const Himax::Pen::PenPressureStats& s,
                                   bool evtRunning,
                                   bool pressRunning,
                                   int16_t sourceIndex,
                                   bool& valid);
    void BuildDebugSchema();

    void ParseServiceConfig(const std::string& configPath);
    void BuildDefaultPipeline(const std::string& configPath);
    Ipc::IpcResponse HandleIpcCommand(const Ipc::IpcRequest& req);
};

} // namespace Service

