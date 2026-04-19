# EGoTouch IPC 接口协议说明

> 最后更新：2026-04-18  
> 状态：已按当前代码实现整理

---

## 1. 概述

EGoTouch 的上位机与服务之间使用两类 IPC 通道：

1. **Named Pipe 命令通道**：负责控制类请求/响应。
2. **Shared Memory + Event 帧通道**：负责调试帧的实时推送。

对应代码位置：

- 协议定义：`Common/include/IpcProtocol.h`
- 共享帧定义：`Common/include/SharedFrameBuffer.h`
- Pipe 客户端：`Common/source/IpcPipeClient.cpp`
- Pipe 服务端分发：`EGoTouchService/source/ServiceHost.cpp`
- 共享内存读写：`Common/source/SharedFrameBuffer.cpp`

---

## 2. 总体通信结构

```text
EGoTouchApp
  ├── IpcPipeClient
  │     └── \\.\pipe\EGoTouchControl
  └── SharedFrameReader
        ├── Global\EGoTouchSharedFrame
        └── Global\EGoTouchFrameReady

EGoTouchService
  ├── IpcPipeServer
  │     └── HandleIpcCommand(...)
  └── SharedFrameWriter
        ├── Global\EGoTouchSharedFrame
        └── Global\EGoTouchFrameReady
```

### 2.1 通道职责划分

| 通道 | 名称 | 方向 | 用途 |
|---|---|---|---|
| Named Pipe | `\\.\pipe\EGoTouchControl` | App ⇄ Service | 控制、查询、配置、日志、动态调试元数据 |
| Shared Memory | `Global\EGoTouchSharedFrame` | Service → App | 实时调试帧推送 |
| Event | `Global\EGoTouchFrameReady` | Service → App | 通知新帧到达 |
| Event | `Global\EGoTouchLogReady` | 预留 | IPC 相关全局事件名 |
| Event | `Global\EGoTouchPenStatusReady` | 预留 | IPC 相关全局事件名 |

---

## 3. Named Pipe 命令协议

## 3.1 Pipe 名称

```cpp
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\EGoTouchControl";
```

客户端通过 `WaitNamedPipeW` + `CreateFileW` 连接，随后将句柄切换到 `PIPE_READMODE_MESSAGE` 消息模式。

## 3.2 请求/响应结构

`Common/include/IpcProtocol.h` 中定义：

```cpp
struct IpcRequest {
    IpcCommand command;
    uint16_t   paramLen = 0;
    uint8_t    param[256]{};
};

struct IpcResponse {
    bool     success = false;
    uint16_t dataLen = 0;
    uint8_t  data[4096]{};
};
```

### 字段说明

#### IpcRequest

| 字段 | 类型 | 说明 |
|---|---|---|
| `command` | `IpcCommand` | 命令编号 |
| `paramLen` | `uint16_t` | `param` 中实际有效字节数 |
| `param` | `uint8_t[256]` | 命令参数区 |

#### IpcResponse

| 字段 | 类型 | 说明 |
|---|---|---|
| `success` | `bool` | 命令是否成功 |
| `dataLen` | `uint16_t` | 返回数据有效字节数 |
| `data` | `uint8_t[4096]` | 返回数据区 |

### 协议约束

- 当前实现为**固定长度结构体整包读写**，不是变长 framing。
- `paramLen` / `dataLen` 仅表示有效负载长度，底层 `WriteFile` / `ReadFile` 仍直接传整个结构体。
- 新增命令时，需同时更新：
  - `Common/include/IpcProtocol.h`
  - `EGoTouchService/source/ServiceHost.cpp:670` 附近分发逻辑
  - 对应 App 侧调用封装

---

## 4. IpcCommand 命令枚举

```cpp
enum class IpcCommand : uint8_t {
    Ping = 0,
    EnterDebugMode = 1,
    ExitDebugMode  = 2,
    StartRuntime   = 10,
    StopRuntime    = 11,
    AfeCommand     = 20,
    SetVhfEnabled   = 30,
    SetVhfTranspose = 31,
    ReloadConfig   = 40,
    SaveConfig     = 41,
    GetLogs        = 50,
    GetPenBridgeStatus = 60,
    GetDebugSchema   = 61,
    GetDebugSnapshot = 62,
};
```

下面按当前实现说明各命令。

### 4.1 Ping

- **命令值**：`0`
- **请求参数**：无
- **返回数据**：无
- **用途**：连通性检测

服务端行为：直接返回 `success = true`。

---

### 4.2 EnterDebugMode

- **命令值**：`1`
- **请求参数**：共享内存名称，`wchar_t[]`
- **返回数据**：无
- **用途**：让 Service 开始向共享内存推送调试帧

App 侧当前通过 `IpcPipeClient::EnterDebugMode()` 发送共享内存名。

注意：

- 当前 Service 端并未实际使用请求里传入的名字，而是依赖已打开的 `SharedFrameWriter`。
- 此功能仅在 `#ifdef _DEBUG` 下启用。
- Release 下会记录 `EnterDebugMode not available in release build.`，并返回失败。

---

### 4.3 ExitDebugMode

- **命令值**：`2`
- **请求参数**：无
- **返回数据**：无
- **用途**：停止调试帧推送

服务端会：

- 清空 `DeviceRuntime` 的 `FramePushCallback`
- 清除最近一帧缓存
- 将 `m_debugMode` 置为 false

---

### 4.4 StartRuntime

- **命令值**：`10`
- **请求参数**：无
- **返回数据**：无
- **用途**：启动 `DeviceRuntime`

服务端行为：调用 `m_deviceRuntime->Start()`，将返回值写入 `success`。

---

### 4.5 StopRuntime

- **命令值**：`11`
- **请求参数**：无
- **返回数据**：无
- **用途**：停止 `DeviceRuntime`

服务端行为：调用 `m_deviceRuntime->Stop()`，成功后返回 `success = true`。

---

### 4.6 AfeCommand

- **命令值**：`20`
- **请求参数**：2 字节
  - `param[0]` = `AFE_Command`
  - `param[1]` = 对应参数
- **返回数据**：无
- **用途**：将 AFE 控制命令转发给运行时命令队列

服务端会构造：

```cpp
command cmd{};
cmd.type = static_cast<AFE_Command>(req.param[0]);
cmd.param = req.param[1];
```

然后通过：

```cpp
m_deviceRuntime->SubmitCommand(cmd, CommandSource::External, "IPC AFE");
```

---

### 4.7 SetVhfEnabled

- **命令值**：`30`
- **请求参数**：1 字节布尔值
- **返回数据**：无
- **用途**：启用/禁用 VHF 输出

---

### 4.8 SetVhfTranspose

- **命令值**：`31`
- **请求参数**：1 字节布尔值
- **返回数据**：无
- **用途**：控制 VHF transpose 模式

---

### 4.9 ReloadConfig

- **命令值**：`40`
- **请求参数**：无
- **返回数据**：无
- **用途**：让服务重新读取 `C:/ProgramData/EGoTouchRev/config.ini`

服务端行为包括：

1. 重新读取 `[Service]` 段。
2. 将配置应用到 `TouchPipeline` / `StylusPipeline`。
3. 如检测到旧格式配置，会迁移并重写 canonical config。

---

### 4.10 SaveConfig

- **命令值**：`41`
- **请求参数**：无
- **返回数据**：无
- **用途**：将当前服务侧配置写回 `config.ini`

---

### 4.11 GetLogs

- **命令值**：`50`
- **请求参数**：无
- **返回数据**：UTF-8/窄字节日志文本，按 `\n` 拼接
- **用途**：拉取 Service 端新增日志

服务端从 `GuiLogSink` 拉取日志行并拼接到 `resp.data`，直到达到 `4096` 字节上限。

---

### 4.12 GetPenBridgeStatus

- **命令值**：`60`
- **请求参数**：无
- **返回数据**：固定 13 字节
- **用途**：查询蓝牙笔桥运行状态与压力统计

返回布局：

```text
[0]  evtRunning
[1]  pressRunning
[2]  reportType
[3]  freq1
[4]  freq2
[5]  press0 low
[6]  press0 high
[7]  press1 low
[8]  press1 high
[9]  press2 low
[10] press2 high
[11] press3 low
[12] press3 high
```

其中 `press0..press3` 为 little-endian `uint16_t`。

---

### 4.13 GetDebugSchema

- **命令值**：`61`
- **请求参数**：`DebugSchemaRequest`
- **返回数据**：`DebugSchemaResponseHeader` + `N * DebugFieldSchemaWire`
- **用途**：获取动态调试字段定义

请求结构：

```cpp
struct DebugSchemaRequest {
    uint16_t offset = 0;
    uint16_t limit = 0;
};
```

返回头：

```cpp
struct DebugSchemaResponseHeader {
    uint16_t schemaVersion = 0;
    uint16_t totalFields = 0;
    uint16_t returnedFields = 0;
    uint16_t recordSize = 0;
    uint32_t schemaHash = 0;
};
```

字段记录：

```cpp
struct DebugFieldSchemaWire {
    uint16_t fieldId;
    uint8_t  valueType;
    uint8_t  sourceKind;
    int16_t  sourceIndex;
    uint8_t  uiOrder;
    uint8_t  dvrTarget;
    uint8_t  dvrPositionMode;
    uint8_t  _reserved0;
    int16_t  dvrIndex;
    int16_t  _reserved1;
    char key[32];
    char displayName[48];
    char unit[16];
    char uiGroup[24];
    char dvrColumnName[32];
    char dvrAnchor[32];
};
```

用于描述：

- 字段 ID
- 值类型
- 值来源（master/slave/stylus/pen/derived）
- UI 展示顺序与分组
- DVR 导出落点

---

### 4.14 GetDebugSnapshot

- **命令值**：`62`
- **请求参数**：无
- **返回数据**：`DebugSnapshotHeader` + `N * DebugSnapshotValueWire`
- **用途**：按当前 schema 返回最近一帧的动态调试值

返回头：

```cpp
struct DebugSnapshotHeader {
    uint16_t schemaVersion = 0;
    uint16_t fieldCount = 0;
    uint16_t recordSize = 0;
    uint16_t _reserved0 = 0;
};
```

值记录：

```cpp
struct DebugSnapshotValueWire {
    uint16_t fieldId = 0;
    uint8_t  valueType = 0;
    uint8_t  flags = 0;   // bit0: value valid
    uint32_t _reserved0 = 0;
    uint64_t rawValue = 0;
};
```

说明：

- `fieldId` 用于和 schema 对应。
- `valueType` 指示解析方式。
- `flags & 0x1` 表示该值有效。
- `rawValue` 为统一编码后的 64 位值：
  - `UInt32`：低 32 位直接存值
  - `Int32`：按 `uint32_t` 位模式写入
  - `Float32`：按 IEEE754 位模式写入低 32 位
  - `Bool`：0/1

---

## 5. 动态调试字段相关枚举

## 5.1 DebugValueType

```cpp
enum class DebugValueType : uint8_t {
    UInt32 = 0,
    Int32  = 1,
    Float32 = 2,
    Bool = 3,
};
```

## 5.2 DebugSourceKind

```cpp
enum class DebugSourceKind : uint8_t {
    MasterSuffixWord = 0,
    SlaveSuffixWord = 1,
    StylusField = 2,
    PenBridgeField = 3,
    DerivedField = 4,
};
```

## 5.3 DebugDvrTarget

```cpp
enum class DebugDvrTarget : uint8_t {
    None = 0,
    MasterStatus = 1,
    SlaveSuffix = 2,
    DynamicDebug = 3,
};
```

## 5.4 DebugDvrPositionMode

```cpp
enum class DebugDvrPositionMode : uint8_t {
    Append = 0,
    AfterAnchor = 1,
    Index = 2,
};
```

## 5.5 DebugStylusSourceIndex

当前定义的 stylus 字段索引：

```cpp
enum class DebugStylusSourceIndex : int16_t {
    Pressure = 0,
    SignalX = 1,
    SignalY = 2,
    MaxRawPeak = 3,
    Status = 4,
    PipelineStage = 5,
    PointX = 6,
    PointY = 7,
    RawPressure = 8,
    MappedPressure = 9,
    NoPressInkActive = 10,
    TouchSuppressActive = 11,
};
```

## 5.6 DebugPenSourceIndex

当前定义的 pen bridge 字段索引：

```cpp
enum class DebugPenSourceIndex : int16_t {
    EvtRunning = 0,
    PressRunning = 1,
    ReportType = 2,
    Freq1 = 3,
    Freq2 = 4,
    Press0 = 5,
    Press1 = 6,
    Press2 = 7,
    Press3 = 8,
};
```

---

## 6. Shared Memory 调试帧协议

## 6.1 资源名称

```cpp
constexpr const wchar_t* kSharedFrameName = L"Global\\EGoTouchSharedFrame";
constexpr const wchar_t* kFrameReadyEventName = L"Global\\EGoTouchFrameReady";
```

## 6.2 设计目标

该通道用于将 Service 内部最新调试帧快速推送到 App，避免每帧走 Pipe 请求/响应。

特点：

- 单向：Service 写，App 读
- 低延迟
- 固定 POD 布局
- 无 STL / 无虚表，可跨进程直接映射
- 使用 **triple buffer** 避免读写竞争

## 6.3 顶层结构

```cpp
struct SharedTripleBuffer {
    static constexpr int kSlotCount = 3;

    alignas(64) std::atomic<uint32_t> readyIdx{0};
    alignas(64) std::atomic<uint64_t> frameId{0};
    alignas(64) std::atomic<uint64_t> slaveFrameId{0};
    alignas(64) std::atomic<uint64_t> masterFrameId{0};

    SharedFrameData slots[kSlotCount]{};
};
```

### 字段说明

| 字段 | 说明 |
|---|---|
| `readyIdx` | 当前可供 Reader 读取的完整槽位 |
| `frameId` | 全局帧计数 |
| `slaveFrameId` | slave 帧计数 |
| `masterFrameId` | master 帧计数，仅 `masterWasRead=true` 时递增 |
| `slots[3]` | 三个 `SharedFrameData` 槽位 |

## 6.4 发布机制

Writer 逻辑：

1. 向 `slots[m_writeIdx]` 写入完整帧。
2. `readyIdx.store(justWritten, release)` 发布该槽。
3. 切换到下一个空闲槽写下一帧。
4. 增加 `frameId/slaveFrameId/masterFrameId`。
5. `SetEvent(kFrameReadyEventName)` 通知 Reader。

Reader 逻辑：

1. 先读取 `frameId`。
2. 若与 `m_lastReadId` 相同，则无新帧。
3. 读取 `readyIdx`。
4. 从该槽位拷贝完整 `SharedFrameData` 到本地 `HeatmapFrame`。

因此 Reader 无需 SeqLock 重试循环。

---

## 7. SharedFrameData 内容布局

`SharedFrameData` 是共享内存中的核心帧对象，包含以下几类信息：

### 7.1 运行状态字段

| 字段 | 说明 |
|---|---|
| `workerState` | Runtime 状态 |
| `streaming` | 是否正在流式运行 |
| `lastFrameProcessUs` | 最近一帧处理耗时 |
| `avgFrameProcessUs` | 平均处理耗时 |
| `acquisitionFps` | 采样 FPS（当前由 App 计算） |
| `slaveAcquisitionFps` | slave FPS（当前由 App 计算） |
| `vhfEnabled` | VHF 是否启用 |
| `vhfDeviceOpen` | VHF 设备是否打开 |
| `vhfTranspose` | VHF transpose 状态 |

### 7.2 热图与时间戳

| 字段 | 说明 |
|---|---|
| `heatmapMatrix[40][60]` | 触摸热图矩阵 |
| `timestamp` | 当前帧时间戳 |

### 7.3 Touch 区域与峰值

| 字段 | 说明 |
|---|---|
| `touchZones[40][60]` | macro zone 可视化数据 |
| `peakZones[40][60]` | peak zone 可视化数据 |
| `peaks[30]` | 峰值列表 |
| `peakCount` | 峰值数量 |

### 7.4 Touch contacts 与包

| 字段 | 说明 |
|---|---|
| `contactCount` | contact 数量 |
| `contacts[10]` | 扁平化 contact 列表 |
| `touchPackets[2]` | 触摸 HID 包 |

### 7.5 Stylus 数据

| 字段 | 说明 |
|---|---|
| `stylusPoint` | 手写笔求解坐标结果 |
| `stylusPacket` | 手写笔 HID 包 |
| `stylusSlaveValid` 等 | stylus 调试字段镜像 |
| `diag` | Stylus pipeline diagnostics |

### 7.6 结构化 suffix

| 字段 | 说明 |
|---|---|
| `masterSuffix` | `Frame::MasterSuffixView` |
| `slaveSuffix` | `Frame::SlaveSuffixView` |
| `masterSuffixValid` | master suffix 是否有效 |
| `slaveSuffixValid` | slave suffix 是否有效 |

---

## 8. 结构化 suffix 协议

共享内存和运行时帧中都直接内嵌了结构化 suffix 视图，定义位于 `Common/include/FrameLayout.h`。

## 8.1 MasterSuffixView

- 大小：`256` 字节
- 字段数：`128` 个 `uint16_t`
- 来源：master frame status table

关键索引：

| Word Index | 名称 | 说明 |
|---|---|---|
| `0` | `kRetryFlag` | 固件要求重试 |
| `2` | `kFreqShiftDone` | 频切完成标志 |
| `3` | `kDiagStatus` | 诊断状态 |
| `6` | `kPendingFreqSwitch` | 待切频请求 |
| `8` | `kTpFreq1` | TP 频点 1 |
| `9` | `kTimestamp` | 帧时间戳 |
| `14` | `kPenF0NoiseCount` | F0 噪声计数 |
| `16` | `kPenF1NoiseCount` | F1 噪声计数 |
| `54` | `kTouchX` | 触摸 X |
| `55` | `kTouchY` | 触摸 Y |

说明：

- 当前 `kTpFreq2` 已与 `kTimestamp` 对齐，实质同索引 `9`。
- `MasterSuffixView::LoadFromBytes()` 按 little-endian 从 SPI 原始字节流装载。

## 8.2 SlaveSuffixView

- 大小：`332` 字节
- 字段数：`166` 个 `uint16_t`
- 布局：`TX1 block (83 words) + TX2 block (83 words)`

每个 block：

- 前 2 个 word：anchor row / col
- 后 81 个 word：`9x9` 网格数据

关键能力：

- `tx1AnchorRow()/tx1AnchorCol()`
- `tx2AnchorRow()/tx2AnchorCol()`
- `tx1Grid(r,c)` / `tx2Grid(r,c)`
- `hasStylus()`

---

## 9. App / Service 交互时序

## 9.1 基础连接

```text
App                       Service
 |                           |
 |-- Connect(pipe) --------->|
 |-- Ping ------------------->|
 |<--------- success ---------|
```

## 9.2 进入调试帧模式

```text
App                                      Service
 |                                          |
 |-- EnterDebugMode(sharedMemName) -------> |
 |                                          |-- SetFramePushCallback(...)
 |                                          |-- SharedFrameWriter.Write(frame)
 |<---------------- success --------------- |
 |                                          |
 |======== wait FrameReadyEvent ==========> |
 |-- SharedFrameReader.Read() ------------> |
```

## 9.3 动态调试字段

```text
App                               Service
 |                                   |
 |-- GetDebugSchema(offset,limit) -->|
 |<-- schema header + records -------|
 |-- GetDebugSnapshot -------------->|
 |<-- snapshot header + values ------|
```

---

## 10. 当前实现注意点

### 10.1 EnterDebugMode 仅 Debug 可用

`ServiceHost.cpp` 中 `EnterDebugMode` 明确放在 `#ifdef _DEBUG` 下，Release 版本不会真正开启共享帧推送。

### 10.2 请求中的共享内存名当前未真正参与选择

虽然 `EnterDebugMode` 的请求负载定义为共享内存名，但当前服务端只检查 `m_frameWriter.IsOpen()`，并未根据请求内容切换目标映射。

### 10.3 Pipe 仍是固定结构体协议

当前没有版本头、魔数、CRC、变长包 framing；兼容性依赖两端结构体定义完全一致。

### 10.4 SharedFrameData 必须保持 POD 稳定布局

新增字段时要注意：

- 不要放入 `std::vector` / `std::string` / 虚函数
- 尽量追加，不要随意调整已有字段顺序
- App 与 Service 需要同步更新

### 10.5 timestamp 分两层含义

当前 IPC 中同时存在：

- `SharedFrameData.timestamp`
- `masterSuffix.timestamp()`

上位机进度条等通用显示通常读取前者；suffix 面板/导出可能直接读取后者。两者在服务侧应保持一致来源。

### 10.6 App 侧接收时间戳

DVR / CSV 现已额外记录 App 侧“收到帧时”的系统时间戳：

- 字段：`HeatmapFrame.receiveSystemEpochUs`
- 单位：`uint64_t` 微秒，自 Unix epoch 起算
- 采集点：`Tools/EGoTouchApp/source/ServiceProxy.Polling.cpp` 中 `m_frameReader.Read(...)` 成功之后立即记录
- 用途：离线分析 App 实际收帧时序，不改变跨进程共享内存协议

注意：该字段 **不在** `SharedFrameData` 中，也不是 Service 生成帧时间；它是 App 本地附加元数据，因此不会破坏共享内存固定 POD 布局。

### 10.7 DVR 导出布局与命名

当前 DVR 导出统一落在：`C:/ProgramData/EGoTouchRev/exports/dvr/`

- Binary DVR：`dvrYYYYMMDD_HHMMSS_xxxxxx.dvrbin`
- 完整 CSV 导出：`dvr<timestamp>/`
- 单帧 CSV：仍按 `manual/` 与 `auto/` 分类，便于即时抓帧

其中 `xxxxxx` 为微秒字段，用于稳定区分高频导出。

### 10.8 DVR2 文件容器

当前 `.dvrbin` 已全面切换到 **DVR2**，旧版 `EGODVRB1` 格式不再兼容导入。

DVR2 采用 **header + TOC + typed sections**：

```cpp
struct Dvr2FileHeader {
    char magic[8];          // "EGODVR2\0"
    uint16_t formatVersion; // 当前为 3（容器/文件格式版本，不是 dynamic debug schemaVersion）
    uint16_t headerSize;
    uint32_t sectionCount;
    uint64_t tocOffset;
    uint32_t flags;
    uint32_t reserved;
};

struct Dvr2SectionEntry {
    uint32_t type;
    uint32_t version;
    uint64_t offset;
    uint64_t size;
};
```

当前必选 section：

- `Meta`：帧总数、frame record version、dataset flags
- `Index`：每帧 timestamp、receiveSystemEpochUs、frameOffset、frameSize
- `Frames`：按稳定语义 schema 写出的 `Dvr2FrameRecordV1`

当 `flags.bit3 = HasDynamicDebug` 时，还要求同时存在：

- `DynamicDebugSchema`：dataset 级 dynamic debug schema，记录 `schemaVersion` / `schemaHash` / 字段列表
- `DynamicDebugValues`：逐帧 dynamic debug 值快照

设计目的：

- 不再把历史 C++ struct 内存布局直接当作长期文件协议
- 后续扩展优先通过新增 section 或升级 section version 完成
- reader 可基于 TOC 做边界校验与定向读取

当前真实兼容边界：

- **已具备**：旧版非 DVR2 文件会被明确识别并拒绝，而不是误读
- **已具备**：同一版 `Dvr2FrameRecordV1` 下，header / TOC / section version / flags 已经独立于运行时 `DvrFrameSlot`
- **尚不具备自动跨版本兼容**：当前 reader 仍要求
  - `Meta` / `Index` / `Frames` 三个必选 section 必须都存在
  - 三个 section 的 `version` 必须都等于 `1`
  - `frameRecordVersion` 必须等于 `1`
  - `header.flags == meta.flags`
  - `framesSection->size == frameCount * sizeof(Dvr2FrameRecordV1)`
  - `index[i].frameSize == sizeof(Dvr2FrameRecordV1)`
  - 若 `HasDynamicDebug` 置位，则 `DynamicDebugSchema` / `DynamicDebugValues` 两个 section 必须都存在，且 version 均为 `1`
  - `DynamicDebugSchema.sectionSize` 必须严格等于 `sizeof(schemaHeader) + fieldCount * sizeof(DebugFieldSchemaWire)`
  - `DynamicDebugValues.frameCount` 必须等于 `Meta.frameCount`
  - 每一帧 dynamic sample 数量、顺序、`fieldId`、`valueType` 都必须与 `DynamicDebugSchema` 严格一致
  - 若未置 `HasDynamicDebug`，则文件中不允许出现 dynamic debug sections
- 这意味着：如果未来直接修改 `Dvr2FrameRecordV1` 大小，或把 `Frames` section 升到 v2，而 reader 还没同步升级，当前实现会**明确拒绝导入**，不会自动兼容

兼容策略：

- 仅接受 `EGODVR2\0` magic
- 若读到旧版 `EGODVRB1`，直接报错拒绝导入
- 不再保留 legacy slot / compat slot 解码路径
- 当前版本的兼容策略是“**强校验 + 明确拒绝**”，不是“新旧版本自动互通”

### 10.9 DVR2 flags 设计

当前 DVR2 header/meta 的 `flags` 仍作为 feature bitset 使用：

- `bit0` = `HasStylusDiagnostics`
- `bit1` = `HasStructuredSuffix`
- `bit2` = `HasReceiveSystemEpochUs`
- `bit3` = `HasDynamicDebug`

设计原则：

- `formatVersion` / section `version` 负责格式演进
- `flags` 负责语义能力标记
- 不再使用 `frameRecordSize` 驱动 legacy 分流
- 不为 App 本地分析字段去修改 `SharedFrameData`

`HasDynamicDebug` 的一致性约束：

- 该 flag 表示“文件中完整持久化了 dynamic debug schema + per-frame values”，不是“某几帧碰巧带了动态值”
- writer 只有在以下条件全部满足时才会置位：
  - 存在非空 dynamic schema
  - schema 内 `fieldId` 唯一
  - 每一帧 dynamic sample 数量与 schema 字段数一致
  - 每一帧 sample 的顺序、`fieldId`、`valueType` 与 schema 严格一致
- reader 见到该 flag 后，会强制校验 schema/values section 是否存在、大小是否合法、并与 `frameCount` / schema 完整对齐
- reader 若未见该 flag，却发现 dynamic debug sections，也会按无效文件拒绝导入

注意区分两个版本号：

- `Dvr2FileHeader.formatVersion`：DVR2 容器版本，当前为 `3`
- `DynamicDebugSchema.schemaVersion`：dynamic debug IPC/schema 版本，来源于 service 侧 `GetDebugSchema` / `GetDebugSnapshot`

如果后续目标是“新 reader 尽量自动兼容未来 writer”，则还需要进一步把 `Frames` section 做成真正的 append-only record schema，或改成字段化/chunked payload；当前代码还没做到这一步。

### 10.10 DVR 目标架构图（升级基线）

下面这部分不是“当前已完全实现”，而是后续升级的目标分层，用来约束代码继续演进时不要再把 transport / runtime / playback / file format 混在一起。

```text
Live IPC Transport
  ├── Named Pipe command channel
  └── SharedFrameData / SharedFrameReader / SharedFrameWriter
          │
          ▼
Runtime Semantic Model
  ├── HeatmapFrame
  ├── TouchPipeline
  └── StylusPipeline
          │
          ▼
Canonical Recording Model
  ├── DvrPlaybackDataset
  ├── DvrPlaybackFrame
  ├── recordingTimeUs / sourceTimeUs / hostReceiveUnixTimeUs
  └── future: RecordingSession / TimelineCursor / PlaybackSnapshot
          │
          ▼
Storage Layer
  ├── DVR2 reader/writer
  ├── section catalog (Meta / Index / Frames)
  └── future: multi-stream / chunked DVR3 container
          │
          ▼
Projection Layer
  ├── DiagnosticsWorkbench
  ├── playback controls
  ├── CSV export
  └── future analysis/export views
```

当前第一阶段已经开始把 playback 从“裸 `vector<HeatmapFrame>`”抬升到 dataset 模型：

- `ServiceProxyTypes.h` 中新增 `DvrPlaybackFrame` / `DvrPlaybackDataset`
- `ServiceProxy.Playback.cpp` 导入后先构造 dataset，再驱动回放
- `ServiceProxy.Dvr.cpp` 导出 CSV 时读取 dataset，而不是直接依赖旧 playback vector

这一步的意义是：

- 先把“回放对象”从底层文件 record 脱钩
- 让后续引入 session / cursor / projector 时，不必继续修改 UI 层接口
- 为后续把时间驱动 playback、multi-stream recording、dynamic debug 持久化纳入同一模型留出生长位

### 10.11 模块职责清单

#### Transport ABI Layer
- 文件：`Common/include/SharedFrameBuffer.h`
- 职责：定义 live shared-memory ABI，只解决 App/Service 之间的实时传输
- 约束：不承载 DVR 持久化语义，不为了离线回放字段演进而修改

#### Runtime Semantic Layer
- 文件：`EGoTouchService/Solvers/SolverTypes.h`
- 职责：定义算法运行期语义对象，例如 `HeatmapFrame`
- 约束：可以承载运行态信息，但不是长期文件协议 authority

#### Canonical Playback / Recording Layer
- 文件：`Tools/EGoTouchApp/include/ServiceProxyTypes.h`
- 职责：承接导入后的回放数据集语义，统一记录时间轴、sequence、dataset flags
- 当前状态：已落第一版 `DvrPlaybackDataset`
- 后续方向：继续提升为 `RecordingSession` / `TimelineCursor` / `PlaybackSnapshot`

#### Storage Layer
- 文件：`Tools/EGoTouchApp/source/ServiceProxy.Dvr.cpp`
- 职责：负责当前 DVR2 container 的读写、section 校验、稳定 record 编解码
- 约束：只负责持久化与恢复，不直接拥有 UI 行为或播放策略

#### Playback Orchestration Layer
- 文件：`Tools/EGoTouchApp/source/ServiceProxy.Playback.cpp`
- 职责：负责 import / play / pause / step / seek，向 UI 提供当前 playback frame
- 当前状态：已从裸 frame vector 切到 dataset 驱动
- 后续方向：从 frame-index 驱动迁移到 time-driven cursor

#### Projection / UI Layer
- 文件：`Tools/EGoTouchApp/source/DiagnosticsWorkbench.Dvr.cpp`
- 职责：显示 dataset 状态、回放控制、导出按钮与时间戳信息
- 约束：不感知底层 file section、record 布局与兼容细节

---

## 11. 维护建议

后续若扩展 IPC，建议优先遵守以下约束：

1. **控制类继续走 Pipe**，不要把高频帧塞进 Pipe。
2. **高频帧继续走 Shared Memory**，保持固定 POD。
3. DVR 文件格式扩展时，优先通过新增 section、提升 section version、补充 feature flag 演进。
4. 动态调试字段新增时，优先扩展 schema/snapshot，而不是硬编码 App UI。

---

## 12. 参考代码位置

- `Common/include/IpcProtocol.h`
- `Common/include/SharedFrameBuffer.h`
- `Common/include/FrameLayout.h`
- `Common/source/IpcPipeClient.cpp`
- `Common/source/SharedFrameBuffer.cpp`
- `EGoTouchService/source/ServiceHost.cpp`
- `Tools/EGoTouchApp/source/ServiceProxy.Connection.cpp`
- `Tools/EGoTouchApp/source/ServiceProxy.Polling.cpp`
- `Tools/EGoTouchApp/source/ServiceProxy.DynamicDebug.cpp`
