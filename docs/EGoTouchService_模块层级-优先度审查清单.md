# EGoTouchService 模块层级-优先度审查清单（不含 Solvers）

> 生成日期：2026-04-19  
> 范围：`EGoTouchService/Device`、`EGoTouchService/Host`、`EGoTouchService/include`、`EGoTouchService/source`  
> 排除：`EGoTouchService/Solvers`（按要求未审查）

## 优先级定义

- **P0（高）**：会持续引入架构/状态一致性风险，建议优先处理。
- **P1（中）**：可维护性和演进风险较高，建议纳入近期迭代。
- **P2（低）**：清晰度/文档/代码卫生问题，建议穿插清理。

---

## 1) `EGoTouchService/Device`

### 1.1 `Device/runtime`

#### P0
1. **[DEV-H01] Runtime 直接耦合 Himax 内部成员（边界泄漏）**  
   - 位置：`EGoTouchService/Device/runtime/DeviceRuntime.cpp:130`、`EGoTouchService/Device/runtime/DeviceRuntime.cpp:181`、`EGoTouchService/Device/runtime/DeviceRuntime.cpp:264`、`EGoTouchService/Device/runtime/DeviceRuntime.cpp:274`、`EGoTouchService/Device/himax/HimaxChip.h:69`  
   - 问题：直接访问 `m_afe` / `back_data` / `m_lastMasterWasRead`，runtime 不再依赖稳定 facade。  
   - 建议：补 `Chip` runtime facade（snapshot/command API），逐步移除内部字段直连。

2. **[DEV-H02] 手写笔状态多源并存（状态语义混杂）**  
   - 位置：`EGoTouchService/Device/runtime/DeviceRuntime.h:29`、`EGoTouchService/Device/himax/HimaxChip.h:16`、`EGoTouchService/Device/common/StylusState.h:6`、`EGoTouchService/Device/vhf/VhfReporter.h:42`、`EGoTouchService/Device/runtime/DeviceRuntime.cpp:344`  
   - 问题：`workerState/ConnectionState/StylusState/m_stylusActive/m_eraserState/PenCurStatus` 并存，无单一真源。  
   - 建议：定义统一 `PenRuntimeState` 并明确 owner 与状态迁移。

3. **[DEV-H04] WorkerMain/OnStreaming 逻辑杂糅**  
   - 位置：`EGoTouchService/Device/runtime/DeviceRuntime.cpp:194`、`EGoTouchService/Device/runtime/DeviceRuntime.cpp:301`  
   - 问题：队列、状态机、恢复、采帧、pipeline、上报、debug 同函数耦合。  
   - 建议：拆为 Acquire/Build/Run/Publish 四段，worker 保留调度职责。

#### P1
4. **[DEV-M06] BT 笔状态链路未闭环/潜在死代码**  
   - 位置：`EGoTouchService/Device/btmcu/PenUsbTypes.h:33`、`EGoTouchService/Device/btmcu/PenUsbTypes.h:37`、`EGoTouchService/Device/vhf/VhfReporter.h:42`  
   - 问题：`PenSessionState`、`EraserToggle` 定义存在但消费链路不完整。  
   - 建议：确认并打通链路；若无用则删除。

#### P2
5. **[DEV-L03] `RecordHistory` 锁语义隐式**  
   - 位置：`EGoTouchService/Device/runtime/DeviceRuntime.cpp:152`  
   - 问题：依赖调用方持锁但函数自身不声明约束。  
   - 建议：注释前置条件或函数内自行加锁。

---

### 1.2 `Device/himax`

#### P0
6. **[DEV-H03] `HimaxChip.cpp` 过于臃肿（God module）**  
   - 位置：`EGoTouchService/Device/himax/HimaxChip.cpp`（约 884 行）  
   - 问题：生命周期、重置、功耗、采帧、恢复、AFE 协作集中在单文件。  
   - 建议：按 lifecycle/frame/boot 先拆 `.cpp`。

#### P1
7. **[DEV-M04] `HimaxProtocol` 混合 HAL 与协议层**  
   - 位置：`EGoTouchService/Device/himax/HimaxProtocol.cpp`、`EGoTouchService/Device/himax/HimaxRegisters.h`  
   - 问题：Win32 I/O 细节与协议语义耦合，寄存器定义平铺过大。  
   - 建议：拆 `HimaxHal`，按功能分组 registers。

8. **[DEV-M05] `ClearStatus` 公开但语义未实现**  
   - 位置：`EGoTouchService/Device/himax/HimaxAfe.cpp:92`、`EGoTouchService/Device/himax/AfeTypes.h:6`  
   - 问题：API 暴露后仅日志处理，调用方会误判能力已生效。  
   - 建议：显式 unsupported（错误码）或补完整实现。

#### P2
9. **[DEV-L01] 文件头注释陈旧**  
   - 位置：`EGoTouchService/Device/himax/HimaxProtocol.h:1`、`EGoTouchService/Device/himax/HimaxProtocol.cpp:1`、`EGoTouchService/Device/himax/HimaxChip.cpp:1`  
   - 问题：仍有旧工程路径/模板占位注释。  
   - 建议：替换为当前模块职责说明。

10. **[DEV-L02] `struct command` 命名过泛**  
    - 位置：`EGoTouchService/Device/himax/AfeTypes.h:25`  
    - 问题：跨文件检索与语义识别差。  
    - 建议：重命名为 `AfeCommandRequest` 等语义名。

11. **[DEV-L04] 协议 helper 重复实现**  
    - 位置：`EGoTouchService/Device/himax/HimaxProtocol.cpp:428`、`EGoTouchService/Device/himax/HimaxChip.cpp:25`  
    - 问题：`himax_parse_assign_cmd` 重复实现。  
    - 建议：收敛到单一 utility。

---

### 1.3 `Device/penevt`

#### P1
12. **[DEV-M02] `PenEventBridge` 设备发现与协议逻辑耦合**  
    - 位置：`EGoTouchService/Device/penevt/PenEventBridge.cpp:29`、`EGoTouchService/Device/penevt/PenEventBridge.cpp:79`、`EGoTouchService/Device/penevt/PenEventBridge.cpp:199`  
    - 问题：SetupDi 枚举、握手、ACK、编码、线程回调混在一类。  
    - 建议：提取 `PenProtocolEncoder/HandshakePolicy`。

---

### 1.4 `Device/vhf`

#### P1
13. **[DEV-M07] `VhfReporter` 同时承担构包与写设备**  
    - 位置：`EGoTouchService/Device/vhf/VhfReporter.cpp:53`、`EGoTouchService/Device/vhf/VhfReporter.cpp:110`  
    - 问题：report builder 与 writer 职责耦合。  
    - 建议：拆 `TouchReportBuilder/StylusPostProcessor` 与 `VhfWriter`。

---

### 1.5 `Device` 公共边界

#### P0
14. **[DEV-H05] 内部模块反向依赖 `Device.h` 聚合头**  
    - 位置：`EGoTouchService/Device/Device.h:2`、`EGoTouchService/Device/himax/HimaxProtocol.h:14`、`EGoTouchService/Device/himax/HimaxChip.h:2`、`EGoTouchService/Device/btmcu/PenUsbTransport.h:3`  
    - 问题：内部实现依赖 facade，扩大编译与概念耦合。  
    - 建议：内部改最小 include，`Device.h` 仅供外层兼容。

#### P1
15. **[DEV-M03] Win32 细节在多个头文件外泄**  
    - 位置：`EGoTouchService/Device/vhf/VhfReporter.h:11`、`EGoTouchService/Device/btmcu/BtHidChannel.h:1`、`EGoTouchService/Device/penpress/PenPressureReader.h:7`、`EGoTouchService/Device/penevt/PenEventBridge.h:5`、`EGoTouchService/Device/himax/HimaxProtocol.h:12`  
    - 问题：`windows.h/HANDLE` 渗透到公共声明层。  
    - 建议：下沉到 `.cpp`，头文件用前置声明/pImpl。

16. **[DEV-M01] 模块级文档缺失**  
    - 位置：`EGoTouchService/Device/`（目录级）  
    - 问题：缺依赖方向、状态机、错误模型文档。  
    - 建议：补 `Device/README.md`（职责图+状态机+边界）。

---

## 2) `EGoTouchService/Host`

### 2.1 `Host/SystemStateMonitor*`

#### P0
17. **[HOST-H01] 回调执行模型不安全**  
    - 位置：`EGoTouchService/Host/SystemStateMonitor.h:16`、`EGoTouchService/Host/SystemStateMonitor.cpp:107`、`EGoTouchService/Host/SystemStateMonitor.cpp:180`  
    - 问题：回调在 worker 线程同步执行，无异常隔离，存在回调重入 `Stop()` 风险。  
    - 建议：定义 callback contract；worker 捕获异常；防 self-join。

18. **[HOST-H02] 事件语义压扁导致状态混杂**  
    - 位置：`EGoTouchService/Host/SystemStateEvent.h:8`、`EGoTouchService/Host/SystemStateMonitor.cpp:200`  
    - 问题：8 个 raw event 归并为较粗状态，调用方可能回退依赖 `raw_index/raw_name`。  
    - 建议：拆分逻辑语义与来源语义，明确映射契约。

19. **[HOST-H03] 公共头暴露 Win32 与 named-event 协议细节**  
    - 位置：`EGoTouchService/Host/SystemStateMonitor.h:10`、`EGoTouchService/Host/SystemStateMonitor.h:42`、`EGoTouchService/Host/SystemStateMonitor.cpp:12`  
    - 问题：`HANDLE` 与事件名策略暴露在 public API。  
    - 建议：pImpl 下沉实现细节，公开平台无关接口。

#### P1
20. **[HOST-M01] 跨进程事件 owner/reset/ACL 契约隐式**  
    - 位置：`EGoTouchService/Host/SystemStateMonitor.cpp:23`、`EGoTouchService/Host/SystemStateMonitor.cpp:48`、`EGoTouchService/Host/SystemStateMonitor.cpp:184`  
    - 问题：`OpenEvent/CreateEvent/ResetEvent` 组合语义未文档化。  
    - 建议：文档明确创建者、重置者、多消费者策略。

21. **[HOST-M02] `SystemStateMonitor` 职责缠结**  
    - 位置：`EGoTouchService/Host/SystemStateMonitor.cpp:23-233`  
    - 问题：同步、ACL、状态翻译、日志、回调调度集中。  
    - 建议：拆 transport adapter + translator + coordinator。

22. **[HOST-M03] 全局事件 ACL 边界偏宽且无说明**  
    - 位置：`EGoTouchService/Host/SystemStateMonitor.cpp:34`、`EGoTouchService/Host/SystemStateMonitor.cpp:48`  
    - 问题：`Global\` + `BU` 权限配置缺少设计说明。  
    - 建议：复核最小权限并文档化理由。

#### P2
23. **[HOST-L01] `SystemStateEventSource` 当前仅单值，抽象收益有限**  
    - 位置：`EGoTouchService/Host/SystemStateEvent.h:18`  
    - 问题：增加认知成本但暂未提供多源价值。  
    - 建议：短期去掉或注释标明 future extension。

24. **[HOST-L02] Host 目录缺少模块 README**  
    - 位置：`EGoTouchService/Host/`（目录级）  
    - 问题：线程模型、事件映射、失败语义不透明。  
    - 建议：补最小契约文档。

---

## 3) `EGoTouchService/include`

### 3.1 `include/ServiceHost.h`

#### P0
25. **[INC-H01] `ServiceHost.h` 成为聚合式 God Header**  
    - 位置：`EGoTouchService/include/ServiceHost.h:3`、`EGoTouchService/include/ServiceHost.h:27`  
    - 问题：runtime/host/penevt/penpress/ipc/shared buffer/solver 全部耦合进 public header。  
    - 建议：前置声明 + pImpl，缩减公开依赖面。

26. **[INC-H02] `ServiceMode + 多布尔开关` 状态模型混杂**  
    - 位置：`EGoTouchService/include/ServiceHost.h:20`、`EGoTouchService/include/ServiceHost.h:44`、`EGoTouchService/include/ServiceHost.h:81`  
    - 问题：`mode/autoMode/stylusVhfEnabled/debugMode/hasLatestDebugFrame` 缺统一不变量。  
    - 建议：拆 `ServiceRuntimeConfig` 与 `ServiceRuntimeState`。

27. **[INC-H03] 顶层 Host 暴露 debug/encoding/IPC 细节（逻辑杂糅）**  
    - 位置：`EGoTouchService/include/ServiceHost.h:63`、`EGoTouchService/include/ServiceHost.h:99`  
    - 问题：`DebugFieldDef`、编码函数、`HandleIpcCommand` 均放在 host 顶层。  
    - 建议：拆 diagnostics registry / encoder / IPC dispatcher。

28. **[INC-H04] 公共头直接暴露完整运行期对象布局**  
    - 位置：`EGoTouchService/include/ServiceHost.h:48`  
    - 问题：句柄、互斥锁、缓存、模块实例全公开在类私有布局。  
    - 建议：以 pImpl 收敛内部资源与缓存。

#### P1
29. **[INC-M03] `ServiceMode` 命名与语义边界不完整**  
    - 位置：`EGoTouchService/include/ServiceHost.h:20`、`EGoTouchService/include/ServiceHost.h:44`  
    - 问题：看似完整模式，实则还受 feature flags 控制。  
    - 建议：重命名并明确 enum 表达范围。

30. **[INC-M04] `DebugFieldDef` 单类型承载过多上下文**  
    - 位置：`EGoTouchService/include/ServiceHost.h:63`  
    - 问题：UI/DVR/source binding 元数据平铺，持续膨胀风险高。  
    - 建议：拆分为 binding/ui/dvr 组合结构。

31. **[INC-M02] 公开 API 契约注释不足**  
    - 位置：`EGoTouchService/include/ServiceHost.h:25`、`EGoTouchService/include/ServiceHost.h:35`、`EGoTouchService/include/ServiceHost.h:99`  
    - 问题：`Start/Stop/GetMode/ParseServiceConfig/HandleIpcCommand` 缺失败语义与线程契约。  
    - 建议：补 pre/postcondition、幂等性、线程安全说明。

32. **[INC-M01] `ServiceShell` 编译期绑定过重**  
    - 位置：`EGoTouchService/include/ServiceShell.h:6`、`EGoTouchService/include/ServiceShell.h:17`  
    - 问题：直接包含 `windows.h + ServiceHost.h` 且对象内嵌 `ServiceHost`。  
    - 建议：前置声明/指针持有，减少壳层耦合。

33. **[INC-M05] include 层缺模块文档**  
    - 位置：`EGoTouchService/include/`（目录级）  
    - 问题：对外边界、分层职责、生命周期规则无 README。  
    - 建议：补 include 层接口契约文档。

#### P2
34. **[INC-L01] 公共头包含较重 STL/实现依赖**  
    - 位置：`EGoTouchService/include/ServiceHost.h:12`  
    - 问题：与当前“完整布局暴露”叠加，放大编译负担。  
    - 建议：pImpl 后再收敛 include。

35. **[INC-L02] `ServiceShell` 生命周期注释不完整**  
    - 位置：`EGoTouchService/include/ServiceShell.h:21`  
    - 问题：SCM 回调线程模型、console/service 路径关系未写清。  
    - 建议：补函数级契约注释。

36. **[INC-L03] `ServiceHost` 下挂 helper 命名层级不匹配**  
    - 位置：`EGoTouchService/include/ServiceHost.h:87`  
    - 问题：`CopyCString/HashDebugSchema/Encode*` 更像独立组件职责。  
    - 建议：迁移到 diagnostics helper/encoder。

---

## 4) `EGoTouchService/source`

### 4.1 `source/ServiceHost.cpp`

#### P0
37. **[SRC-H03] `ServiceHost.cpp` 过度集中（God file）**  
    - 位置：`EGoTouchService/source/ServiceHost.cpp`（约 973 行）  
    - 问题：配置迁移、启动编排、IPC、debug schema、BT 笔通道全部集中。  
    - 建议：拆 `ConfigStore`、`IpcDispatcher`、`DebugRegistry`、`LifecycleCoordinator`。

38. **[SRC-H01] 配置热更新与运行拓扑收敛不一致（状态混杂）**  
    - 位置：`EGoTouchService/source/ServiceHost.cpp:224`、`EGoTouchService/source/ServiceHost.cpp:239`、`EGoTouchService/source/ServiceHost.cpp:309`、`EGoTouchService/source/ServiceHost.cpp:750`  
    - 问题：`ReloadConfig` 只更新部分维度（如 `stylus_vhf_enabled`），`mode/auto_mode` 与 BT 模块拓扑可能不同步。  
    - 建议：统一 `ApplyServiceConfiguration()` 收敛入口。

#### P1
39. **[SRC-M01] 启动与热加载配置迁移逻辑重复**  
    - 位置：`EGoTouchService/source/ServiceHost.cpp:662`、`EGoTouchService/source/ServiceHost.cpp:773`  
    - 问题：legacy 检测、备份、重写 canonical 在两处重复。  
    - 建议：提炼单一迁移 helper。

40. **[SRC-M02] Debug schema/取值/封包三处同步风险**  
    - 位置：`EGoTouchService/source/ServiceHost.cpp:469`、`EGoTouchService/source/ServiceHost.cpp:547`、`EGoTouchService/source/ServiceHost.cpp:861`  
    - 问题：字段新增需改多处，易出现 schema 与 snapshot 漂移。  
    - 建议：以 descriptor registry 单源派生 schema+snapshot。

41. **[SRC-M04] source 目录缺模块文档**  
    - 位置：`EGoTouchService/source/`（目录级）  
    - 问题：启动顺序、IPC 副作用、热更新矩阵无统一文档。  
    - 建议：补 source README（生命周期+协议矩阵）。

#### P2
42. **[SRC-L01] 路径与设备 endpoint 硬编码分散**  
    - 位置：`EGoTouchService/source/ServiceEntry.cpp:13`、`EGoTouchService/source/ServiceHost.cpp:25`、`EGoTouchService/source/ServiceHost.cpp:28`  
    - 问题：环境差异策略分散维护。  
    - 建议：收敛到统一环境配置提供器。

---

### 4.2 `source/ServiceShell.cpp`

#### P0
43. **[SRC-H02] 通过 `NamedEventList()[idx]` 的数字下标耦合协议**  
    - 位置：`EGoTouchService/source/ServiceShell.cpp:54`、`EGoTouchService/source/ServiceShell.cpp:67`、`EGoTouchService/source/ServiceShell.cpp:76`、`EGoTouchService/source/ServiceShell.cpp:97`  
    - 问题：顺序契约隐式、脆弱，改序即错。  
    - 建议：改强类型枚举/显式事件 API。

#### P1
44. **[SRC-M03] 注册了 away GUID 但未形成业务处理语义**  
    - 位置：`EGoTouchService/source/ServiceShell.cpp:72`、`EGoTouchService/source/ServiceShell.cpp:183`  
    - 问题：注册与日志已存在，但控制路径未完整处理。  
    - 建议：要么实现语义，要么移除注册避免误导。

---

### 4.3 `source/ServiceEntry.cpp`

#### P2
45. **[SRC-L02] 入口文件混合安装/运行时策略职责**  
    - 位置：`EGoTouchService/source/ServiceEntry.cpp:19`、`EGoTouchService/source/ServiceEntry.cpp:120`  
    - 问题：install/uninstall、目录初始化、优先级、SCM 启动都在 `wmain` 相关路径。  
    - 建议：拆 installer/CLI 模块，入口只做参数分发。

---

## 建议执行顺序（跨模块）

### 第一批（先做，架构止血）
1. DEV-H01 / DEV-H02 / DEV-H04（`DeviceRuntime` 边界与状态模型）
2. SRC-H01 / SRC-H03（`ServiceHost.cpp` 配置-拓扑收敛与拆分）
3. SRC-H02 + HOST-H02（系统事件契约强类型化，去下标耦合）
4. HOST-H01（callback 线程/异常/重入契约）
5. INC-H01/H04（`ServiceHost.h` 收口，pImpl 降耦）

### 第二批（中期稳态）
1. DEV-H03 + DEV-M04（Himax 模块拆分）
2. DEV-M02 + DEV-M07（BT 笔桥接与 VHF 报告拆责）
3. SRC-M02 + INC-M04（debug schema 单源化）
4. HOST-M01/M02/M03（Host 协议与权限边界文档化）

### 第三批（代码卫生与文档）
1. 所有 P2 项
2. 目录级 README（`Device/Host/include/source`）
3. 统一补 API contract（Start/Stop/Reload/IPC 副作用）
