# TSA_MSProcess 手指解算管线架构分析

> 逆向对象：`TSACore.dll`（image base `0x6ba40000`，x86-64）
> 入口函数：`TSA_MSProcess @ 0x6ba90e43`
> 项目预设：`W273AS2700`（Gaokun Himax CSOT，40 RX × 60 TX）
> 配置来源：`g_tsaPrmtFlashGaokunHimaxCSOT`（经 `tsaprmt-table-reader` 实测确认）

---

## 1. 概述

`TSA_MSProcess` 是**互容（Mutual-Sensing）手指触摸**的逐帧解算管线入口，由上层逐帧驱动 `TSA_Processing` 在 `FLAG_SKIP_MS_PROCESS` 未置位时调用。它把 AFE 采集的一帧 **40×60 互容原始矩阵**，经过「原始预处理 → 基线差分/共模滤除/网格 IIR → 峰值检测 → 触摸跟踪与 ID 分配 → 事件生成 → 上报」一整条链路，最终产出可上报的触点集合。

本机当前预设下，管线呈现一条**精简的有效路径**，多个特性分支在 Flash 参数中被关闭。下表是经实测确认的关键开关：

| 参数 | 偏移 | 实测值 | 含义 / 对管线的影响 |
|------|------|--------|---------------------|
| `bCols` | `0x1d` | `0x3C` = 60 | 传感器列数（TX）|
| `bRows` | `0x1e` | `0x28` = 40 | 传感器行数（RX）|
| `wBufElementCount` | `0x2c` | `0x0960` = 2400 | 网格元素总数（60×40），所有缓冲拷贝以它为长度 |
| `dwPeakProcessingMode` | `0x34` | `3` | 峰值检测走 **Mode 3**（Z8→全内嵌SD判定→Z1）|
| `bCmfEnabled` | `0x68` | `0` | CMF **关闭** → 走 fallback：`Rawdata_CMF` 跳过，改在后段 `CMF_Process` 运行 |
| `bCmfForceDim1Pass` | `0x19` | `1` | CMF 强制**一维通道**（不做 2D 滤波）|
| `dwCmfMethodNormal` | `0x810` | `0` | 正常场景 CMF method=0 → 走 `CMF_ProcessDim`（非 `CMF_Filter2D`）|
| `dwCmfMethodSmartCover` | `0x814` | `2` | 智能保护套场景才用 method 2 |
| `wPeakMode3ThresholdMax` | `0x5b6` | `0x0258` = 600 | Mode 3 动态阈值上限，运行时用其 **一半 (300)** 做钳制 |
| `wPeakFilterReferenceThreshold` | `0x5b4` | `0x0BB8` = 3000 | 峰值滤波参考阈值 |
| `dwAftModeFeatureMask` | `0x60` | `0` | AFT 模式掩码为 0 |
| `bRawdataForceNormalize` | `0x604` | `0` | 不强制归一化 |

> **运行时禁用的分支**（来自反编译条件判定，非 Flash 静态值）：`Rawdata_CMF`、`SafeBaseline_*`、`Exception_CheckUnderWater`、`SignalDisparity_PostProcess`、`AFT_Process`、`SmartCover_Process`。这些都受 `dwFeatureFlags` 运行时位控制，当前路径不进入。

---

## 2. 管线在系统中的位置

`TSA_MSProcess` 不是最外层入口，它被逐帧驱动 `TSA_Processing @ 0x6ba9a623` 包裹。手指解算（MS）与手写笔解算（ASA）在同一帧内顺序执行——**ASA 链路（手写笔）不在本报告范围内**。

```mermaid
flowchart TD
    AFE["AFE 采集<br/>40×60 互容原始帧"] --> TP["TSA_Processing<br/>逐帧管线入口"]
    TP --> FOLD["TSAFold_InputProcess<br/>+ AFE_PreProcess"]
    FOLD --> GATE{"FLAG_SKIP_MS_PROCESS?"}
    GATE -->|未置位| MS["TSA_MSProcess<br/>★ 手指解算管线 ★"]
    GATE -->|置位| SKIP["跳过 MS"]
    MS --> SIDE["SideTouch / Thenar / SHB"]
    SKIP --> SIDE
    SIDE --> ASACHK{"FEAT_ASA_ENABLED?"}
    ASACHK -->|是| ASA["TSA_ASAProcess<br/>（手写笔坐标修正，本报告不展开）"]
    ASACHK -->|否| OUT
    ASA --> OUT["TSAOut_PostProcess<br/>DoubleReport / DMD / 帧计数"]
    OUT --> DONE["bFrameDoneFlag=1<br/>AFE_Process / Fold 输出"]

    style MS fill:#2563eb,color:#fff
    style ASA fill:#9ca3af,color:#000
```

---

## 3. TSA_MSProcess 总体流程

整条管线分为 6 个逻辑阶段。下图展示主路径与三条提前退出/保持上一帧的旁路。

```mermaid
flowchart TD
    START["TSA_MSProcess(pMutualRawData)"] --> SETFLAG["置 PROC_MS_ACTIVE<br/>DataSwitch_ToGrid()<br/>AFE_GetFrame + SS_CopyRaw"]
    SETFLAG --> NULLCHK{"raw==NULL 或<br/>SS_IsRawValid 失败?"}

    NULLCHK -->|是| ABORT["HW Reset 判定 → 可能 TSA_MSReset<br/>条件性 SS_Process<br/>TSA_MSProcessEnding ⏹"]

    NULLCHK -->|否| PREP["① 预处理<br/>CleanPointXY / HardwareAnalyzer_Reset<br/>Proximity_Process / Touch_Clean<br/>备份 preDif/preRaw"]
    PREP --> RAWCHK["TSA_RawCheckProcess"]
    RAWCHK --> DIFPATH{"FLAG_USE_DIF_BUF_PATH?"}

    DIFPATH -->|否 常规| TORAW["raw → g_tsaBufRaw<br/>TPSensor_ProcessRaw"]
    DIFPATH -->|是 旁路| TODIF["raw → g_tsaBufDif<br/>保持上一帧触点<br/>BLIIR 差分 → Ending ⏹"]

    TORAW --> RAWPROC["Rawdata_Process<br/>HardwareAnalyzer_Process"]
    RAWPROC --> BYPASS1{"TSA_IsToBypassCurrentFrame?"}
    BYPASS1 -->|是| ENDBYP["TSA_MSProcessEnding ⏹"]

    BYPASS1 -->|否| SIG["② 信号处理层<br/>（见第 4 节）"]
    SIG --> CHG["③ 充电噪声检查点<br/>（见第 5 节）"]
    CHG --> KEEP{"保持上一帧?"}
    KEEP -->|是| KEEPPREV["Touch/SideTouch KeepPrev<br/>TSA_MSProcessEnding ⏹"]
    KEEP -->|否| PEAK["④ 峰值检测<br/>（见第 6 节）"]
    PEAK --> TRACK["⑤ 触摸跟踪与事件<br/>（见第 7 节）"]
    TRACK --> REPORT["⑥ 上报与收尾<br/>（见第 8 节）"]
    REPORT --> END["TSA_MSProcessEnding ⏹"]

    style SIG fill:#0891b2,color:#fff
    style PEAK fill:#7c3aed,color:#fff
    style TRACK fill:#c026d3,color:#fff
    style REPORT fill:#059669,color:#fff
```

---

## 4. 信号处理层（baseline / CMF / IIR）

进入信号处理层前先调用 `TSAPrmt_PreProcess` 与 `SS_Process`（自容/Side 通道的差分、IIR、CMF、属性提取）。随后是手指主通道的差分与滤波链。**当 `FLAG_USE_DIF_BUF_PATH` 未置位时**走完整链路：

```mermaid
flowchart TD
    A["SS_Process<br/>自容通道：基线/差分/CMF/属性"] --> B["备份 g_tsaBufRaw → preCMFRaw"]
    B --> C["BLIIR_CalcDiffCommon<br/>dif = baseline − raw（或反向）"]
    C --> D["TSAPrpt_GetDifPreCMFPrpt<br/>BLSM_UpdateForRawUnstable"]
    D --> E{"bCmfEnabled?"}
    E -->|=0 当前| E2["跳过 Rawdata_CMF"]
    E -->|≠0| E1["Rawdata_CMF"]
    E2 --> F["BLSM_Process<br/>基线状态机"]
    E1 --> F
    F --> G{"BLSM_IsReset?"}
    G -->|否| H{"bCmfEnabled==0?"}
    H -->|是 当前| H1["CMF_Process<br/>共模噪声滤除（fallback 位置）"]
    H1 --> I["GridIIR_Process<br/>网格时域 IIR"]
    H -->|否| I
    G -->|是| J
    I --> J["HardwareAnalyzer_ProcessDif<br/>TPSensor_Process"]
    J --> K["TSA_GetPrpt<br/>raw/dif 属性提取"]
    K --> L["Self_Process<br/>ToeSynaBl_GetSidePrpt<br/>SS_CheckDirtyByMutual"]

    style C fill:#0891b2,color:#fff
    style F fill:#0891b2,color:#fff
    style H1 fill:#0891b2,color:#fff
    style I fill:#0891b2,color:#fff
```

### 4.1 BLIIR_CalcDiffCommon（差分）`ordinal 346`

逐元素计算差分图，方向由 `g_tsaStaticPtr->bField_0x11a` 决定，长度为 `wBufElementCount`（2400）：

```
bField_0x11a == 0 : dif[i] = baseline[i] − raw[i]   （常规）
bField_0x11a != 0 : dif[i] = raw[i] − baseline[i]   （反向）
```

### 4.2 BLSM_Process（基线状态机）`ordinal 380`

基线管理核心，依次执行四步子流程，并根据**硬件复位**与**强制刷新**两个入参/静态锁存位选择基线属性：

```mermaid
flowchart LR
    R["BLRecal_Process<br/>重校准"] --> P["BLSM_GetProperty<br/>(reset, refresh)"]
    P --> U["BLSM_UpdateStage<br/>阶段推进"]
    U --> S["BLSM_ProcessStage<br/>阶段执行"]
    S --> H["BLSM_ShbProcess<br/>SHB 处理"]
```

- `bForceBaselineProperty` 或静态位 `(*g_tsaStaticPtr)[1].field_0x2` → 触发属性刷新
- `bHardwareReset` 或静态位 `(*g_tsaStaticPtr)[1].field_0x1` → 触发基线复位属性
- `TSA_MSProcess` 中按 `TSAStatic_IsHWReset()` 结果传入 `BLSM_Process(true,false)` 或 `BLSM_Process(false,false)`

### 4.3 CMF_Process（共模噪声滤除）`ordinal 478`

在 `bCmfEnabled=0` 的当前配置下，CMF 不在 `Rawdata_CMF` 阶段执行，而是作为 **fallback 落到 BLSM 之后**。其内部分支由 `CMF_GetCmfMethod()` 决定：

```mermaid
flowchart TD
    A["备份 dif → preCMFDif<br/>CMF_SaveOrderDiff / SavePreCmfDif"] --> B{"bCmfProcessingBlocked?"}
    B -->|是| Z["直接返回"]
    B -->|否| C{"CMF_GetCmfMethod()"}
    C -->|method==1| D["CMF_Filter2D<br/>二维滤波"]
    C -->|method==0 当前| E["CMFRemap_Enter<br/>清零 commonModeNoise 缓冲"]
    E --> F{"bCmfForceDim1Pass==0<br/>且 不需 Remap?"}
    F -->|是| G["CMF_ProcessDim(2)<br/>二维通道"]
    F -->|否 当前| H["CMF_ProcessDim(1)<br/>一维通道"]
    G --> I["Baseline_Compensation<br/>(静态位允许时)"]
    H --> I
    I --> J["CMFRemap_Exit"]
    J --> K{"Game 场景 且 dyn[0x40]?"}
    K -->|是| L["CMF_ProcessDimCommonNoise(1)"]
    K -->|否| M["备份 dif → AftDim1CMFDif / AftCMFDif"]
    L --> M

    style H fill:#0891b2,color:#fff
```

> 实测 `dwCmfMethodNormal=0` + `bCmfForceDim1Pass=1` ⇒ 当前帧固定走 **`CMF_ProcessDim(1)` 一维通道**，二维滤波路径在普通场景不触发。

### 4.4 GridIIR_Process（网格时域 IIR）`ordinal 637`

仅在频点判定为噪声时介入，对 dif 做时域平滑：

```
TSAStatic_IsAllFreqBecomeNoisy() == true  : 直接 memcpy(dif → GridIIRDif)，不滤波
否则 IsAllFreqNoisy() == true              : GridIIR_ProcessCore(dif, dif, GridIIRDif, ...)
两者皆否                                    : 不处理
```

IIR 系数取自 `g_tsaPrmtFlash[1].pDim1PitchMap + 3`（运行时滤波参数字节）。

---

## 5. 充电噪声检查点

信号处理后、峰值检测前，管线判定是否因充电器噪声或 bypass 而**保持上一帧触点**：

```mermaid
flowchart TD
    A["TSA_MSRawDirectionDectect"] --> B["bKeepPrev = false<br/>Exception_CheckChargerNoiseInRxLines"]
    B --> C{"prevTouches.nTouchCount != 0x100<br/>且 充电噪声已检出?"}
    C -->|是| D["bKeepPrev = true<br/>Exception_Process"]
    C -->|否| E{"TSA_IsToBypassCurrentFrame?"}
    D --> E
    E -->|是| F["bKeepPrev = true"]
    E -->|否| G[继续]
    F --> H{"bKeepPrev?"}
    G --> H
    H -->|是| KEEP["Touch_KeepPrevTouchWithExceptionExcluded<br/>SideTouch_KeepPrev...<br/>TSA_MSProcessEnding ⏹"]
    H -->|否| PEAK["进入峰值检测"]

    style KEEP fill:#dc2626,color:#fff
```

---

## 6. 峰值检测层

```mermaid
flowchart TD
    A["SS_ChargerNoiseFilterProcess<br/>TSAPrmt_Process"] --> B["SignalDisparity_Process<br/>仅计算 SD 缩放系数"]
    B --> C["Peak_Process ①<br/>峰值检测主体"]
    C --> D["备份 peaks → peaksBeforeSwitchDifBuffer"]
    D --> E["SS_ChargerNoiseFilterSwitchBuffer()<br/>SignalDisparity_SwitchBuffer()"]
    E --> F{"任一 SwitchBuffer 返回非0?"}
    F -->|是| G["Peak_Process ②<br/>切换 dif 缓冲后重新检测"]
    F -->|否| H
    G --> H["TSA_MSPeakFilter<br/>Exception_CheckPanelSD"]
    H --> I["GripFilter_RegionProcess<br/>(置 grip 0x400)"]

    style C fill:#7c3aed,color:#fff
    style G fill:#7c3aed,color:#fff
```

### 6.1 Peak_Process（峰值检测主体）`ordinal 992`

当前 `dwPeakProcessingMode=3`，核心步骤：

```mermaid
flowchart TD
    A["读取动态阈值 dyn+8"] --> B{"Mode==3 且<br/>阈值 > wPeakMode3ThresholdMax/2 (300)?"}
    B -->|是| C["阈值钳制到 300"]
    B -->|否| D
    C --> D["Peak_IsToExcludeSideRegion<br/>计算检测行列边界"]
    D --> E["Peak_DetectInRange<br/>在范围内找局部极大值"]
    E --> F{"Mode==3?"}
    F -->|是| G["Peak_Z8Filter<br/>8 邻域峰值滤波"]
    G --> H{"Peak_IsFullIncellSDDetected?"}
    H -->|否| I["清 bFullIncellSignalDisparity<br/>Peak_Z1Filter(原阈值)<br/>恢复原阈值"]
    H -->|是| J["置 bFullIncellSignalDisparity<br/>阈值 = dyn[0x1c]×阈值 >> 4"]
    I --> K["统计最小峰值行 → peaks[0xc]"]
    J --> K
    K --> L{"needUDFPLargeFingerWorkaround?"}
    L -->|否| M["Peak_SortInSigAscending<br/>信号升序排序"]
    L -->|是| N["Peak_SortInSigDescending<br/>信号降序排序"]
    M --> O["Peak_IDTracking<br/>峰值级 ID 跟踪"]
    N --> O

    style G fill:#7c3aed,color:#fff
    style O fill:#7c3aed,color:#fff
```

> Mode 2 路径（非当前）：当 `SignalDisparity_IsOKToProcess` 且峰值数 > 2 时，用 `Peak_GetMaxPeak()/3` 再做一次 Z1 滤波。

### 6.2 SignalDisparity_Process（信号差异）`ordinal 1419`

当前仅**计算 SD 缩放系数**并取两种估计的较小值写入静态区；因 `FEAT_SIGNAL_DISPARITY_POST` 关闭，`SignalDisparity_Compensation` 补偿与后处理均跳过：

```
scale = min( GetSDScalingByScoring(), GetSDScalingBySignalRatio() )
→ 写入 (*g_tsaStaticPtr)[1].field_0x12
```

---

## 7. 触摸跟踪与事件层

这是把「峰值」转换为「带稳定 ID 的触点轨迹」并生成 down/move/up 事件的核心。触点结构步长 `0x168`（360B），上一帧触点步长 `0x478`（1144B）。

```mermaid
flowchart TD
    A["TSABuffer_PrevFrameProcess"] --> B["TZ_Process<br/>峰值 → 触摸区(TouchZone)"]
    B --> C["TSA_MSTouchPreFilter"]
    C --> D["CTD_ECProcess<br/>边缘坐标补偿 + EdgeDist"]
    D --> E["IDT_Process(0)<br/>ID 跟踪"]
    E --> F["Touch_AssignPreIdxBasedOnID<br/>PrevTouch_AssignCurIdxBasedOnID<br/>建立 cur↔prev 索引映射"]
    F --> G["TS_Process<br/>逐触点状态/坐标解算"]
    G --> H["TE_Process<br/>触摸事件 down/move/up"]
    H --> I["TSABuffer_PreProcess<br/>ER_Process"]

    style B fill:#c026d3,color:#fff
    style E fill:#c026d3,color:#fff
    style G fill:#c026d3,color:#fff
    style H fill:#c026d3,color:#fff
```

### 7.1 TZ_Process（触摸区生成）`ordinal 2439`

把当前峰值转换为触摸区；当 grip flag `0x200` 置位时额外做峰值 TZ 的年龄（age）处理：

```
if (gripFlags & 0x200) TZ_UpdatePeakTzAge();
touches.count = 0;
TZ_PeakBasedProcess();          // 基于峰值建立触摸区
if (gripFlags & 0x200) TZ_AgeProcess();
```

### 7.2 IDT_Process（ID 跟踪）`ordinal 886`

用 20 字节临时映射表，串联三步实现帧间 ID 关联：

```mermaid
flowchart LR
    A["IDT_HMProcess<br/>匈牙利/匹配算法<br/>(cur↔prev 关联)"] --> B["IDT_AccCheck<br/>加速度合理性检查"]
    B --> C["IDT_Map<br/>当前触点映射到 ID"]
```

### 7.3 TS_Process（触点状态解算）`ordinal 2349`

逐触点调用 `TS_ProcessUnit(prevIdx, curIdx)` 完成坐标/状态精算；跳过状态位 `0x20`（offset `+0x21c`）已置位的触点。

### 7.4 TE_Process（触摸事件）`ordinal 1461`

生成 down/move/up 事件。**当前 WindowsPad 特性启用**，因此抬起走 `TE_WindowsLiftOffProcess`：

```mermaid
flowchart TD
    A["遍历 prevTouches"] --> B{"当前帧无匹配<br/>(mappedIdx==0x14)?"}
    B -->|是| C{"WindowsPad 启用?"}
    C -->|是 当前| D["TE_WindowsLiftOffProcess<br/>抬起事件"]
    C -->|否| E["TE_LiftOffProcess"]
    A2["遍历 curTouches"] --> F{"是新触点?"}
    F -->|是 down标志=0| G["TE_TouchDownProcess"]
    F -->|否| H{"prev 防抖位?"}
    H -->|否| I["TE_MoveProcess"]
    H -->|是| J["TE_TouchDownDebounce"]
    G --> K["TE_AssertProcess<br/>异常则 Touch_Clean+PrevTouch_Release"]
    I --> K
    J --> K
    D --> K
    E --> K
    K --> L{"PROC_MS_ACTIVE?"}
    L -->|是| M["Touch_ProcessAftTouchEvent<br/>TSAStatic_ProcessAftTouchEvent"]

    style D fill:#c026d3,color:#fff
```

---

## 8. 上报与收尾层

```mermaid
flowchart TD
    A["SafeBaseline_Process<br/>(FEAT_SAFE_BASELINE 关闭 → 跳过)"] --> B["TouchAction_Process<br/>TSA_MSTouchPostFilter"]
    B --> C["TS_PostProcess / TouchAction_PostProcess<br/>Exception_Process"]
    C --> D["Gesture_Process<br/>GripFilter_Process<br/>TouchMode_Process"]
    D --> E["SignalDisparity_PostProcess<br/>(FEAT_..._POST 关闭 → 跳过)"]
    E --> F["SS_ChargerNoiseFilterPostProcess<br/>TSAIDE_LogPreFltGridRpt"]
    F --> G["AntiTouch_Process<br/>Touch_ProcessBetaGrip"]
    G --> H["TouchReport_Process ★<br/>构建最终触摸上报单元"]
    H --> I["Touch_ProcessExtInfo<br/>HardwareAnalyzer_PostProcess<br/>HandGesture_Process"]
    I --> J["BigData_RecordProcess<br/>BigData_StatProcess"]
    J --> K["PrevTouch_PostProcess<br/>PrevPeak_Process<br/>TSABuffer_PostProcess"]
    K --> L["TSA_MSProcessEnding ⏹"]

    style H fill:#059669,color:#fff
```

### 8.1 TouchReport_Process（最终上报）`ordinal 2501`

逐触点调用 `TouchReport_ProcessUnit(curIdx, prevIdx)` 生成上报单元；当 multi-step-back 启用且**全部触点正在回退**时，用上一帧坐标（prev offset `+0x594/+0x596`）回填当前触点坐标（`+0x170/+0x172`），避免回退抖动。

---

## 9. 完整数据缓冲流转

下图汇总一帧数据在各全局缓冲间的流转（缓冲名取自 `_refptr_g_tsaBuf*`）：

```mermaid
flowchart LR
    RAW["AFE Frame"] -->|SS_CopyRaw| GR["g_tsaBufRaw"]
    GR -->|"raw → preRaw 备份"| PR["g_tsaBufPreRaw"]
    GR -->|BLIIR_CalcDiffCommon<br/>baseline−raw| DIF["g_tsaBufDif"]
    BL["g_tsaBufBl<br/>(基线)"] --> DIF
    DIF -->|备份| PCD["g_tsaBufPreCMFDif"]
    DIF -->|"CMF_ProcessDim(1)"| DIF
    DIF -->|GridIIR| GIIR["g_tsaBufGridIIRDif"]
    DIF -->|Peak_DetectInRange| PK["g_peaks (步长0x14)"]
    PK -->|TZ_Process| TZ["g_touches (步长0x168)"]
    TZ -->|IDT/TS/TE| TZ
    TZ -->|TouchReport| RPT["上报触点集"]
    TZ -.帧末.-> PREV["g_prevTouches (步长0x478)"]

    style DIF fill:#0891b2,color:#fff
    style PK fill:#7c3aed,color:#fff
    style TZ fill:#c026d3,color:#fff
    style RPT fill:#059669,color:#fff
```

---

## 10. 关键函数索引

| 阶段 | 函数 | 相对偏移 | ordinal | 职责 |
|------|------|---------|---------|------|
| 入口 | `TSA_Processing` | `0x5a443` | 2122 | 逐帧管线驱动（MS + ASA + 输出）|
| 入口 | `TSA_MSProcess` | `0x50e43` | — | 手指互容解算管线主体 |
| 信号 | `BLIIR_CalcDiffCommon` | `0x6f4c` | 346 | 基线差分图计算 |
| 信号 | `BLSM_Process` | — | 380 | 基线状态机（重校准/属性/阶段/SHB）|
| 信号 | `CMF_Process` | `0xf2c8` | 478 | 共模噪声滤除（当前一维通道）|
| 信号 | `GridIIR_Process` | `0x21098` | 637 | 噪声帧网格时域 IIR |
| 信号 | `SS_Process` | `0xc0e4e` | 1202 | 自容通道差分/IIR/CMF/属性 |
| 信号 | `TSA_GetPrpt` | `0x61353` | 1975 | raw/dif 属性提取 |
| 峰值 | `Peak_Process` | `0x28c4b` | 992 | 峰值检测主体（Mode 3）|
| 峰值 | `SignalDisparity_Process` | `0xf5d84` | 1419 | 信号差异缩放系数 |
| 跟踪 | `TZ_Process` | `0x4c9f0` | 2439 | 峰值 → 触摸区 |
| 跟踪 | `CTD_ECProcess` | `0x15ca0` | 490 | 边缘坐标补偿 |
| 跟踪 | `IDT_Process` | `0x211a6` | 886 | 帧间 ID 跟踪 |
| 跟踪 | `TS_Process` | `0x3c2cb` | 2349 | 触点状态/坐标解算 |
| 跟踪 | `TE_Process` | `0x389d9` | 1461 | 触摸事件 down/move/up |
| 上报 | `TouchReport_Process` | `0x3be73` | 2501 | 构建最终上报单元 |

> 相对偏移取自各函数反编译头部 `/* <偏移> <ordinal> <名称> */` 注释；`TSA_MSProcess` 绝对地址 `0x6ba90e43`，叠加 image base `0x6ba40000` 反推相对偏移 `0x50e43`。`TSA_Processing` 绝对地址 `0x6ba9a623`。

---

## 11. 小结

当前 `W273AS2700` 预设下，手指解算管线是一条**经过裁剪的高效路径**：

1. **差分主导**：`BLIIR_CalcDiffCommon` 产出差分图，基线由 `BLSM` 状态机维护。
2. **CMF 走 fallback 一维通道**：`bCmfEnabled=0` 使共模滤除推迟到 BLSM 之后，且 `bCmfForceDim1Pass=1` + `method=0` 锁定为 `CMF_ProcessDim(1)`。
3. **峰值 Mode 3**：动态阈值钳制（上限 300）+ Z8 邻域滤波 + 全内嵌 SD 判定 + Z1 滤波 + 峰值级 ID 跟踪。
4. **双次峰值检测**：充电噪声/SD 切换缓冲后会重跑一次 `Peak_Process`。
5. **跟踪链完整**：TZ → 边缘补偿 → IDT(匈牙利匹配+加速度检查) → TS → TE(WindowsPad 抬起)。
6. **多特性关闭**：UnderWater / SafeBaseline / SignalDisparity-Post / AFT / SmartCover 均不在当前路径。

所有 Flash 静态开关均经 `tsaprmt-table-reader` 实测，与反编译条件判定一致。
