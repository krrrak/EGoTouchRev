# TouchPipeline 手指解算管线 — 完整架构与算法流程

> 基于 [TouchPipeline.h](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchPipeline.h) / [TouchPipeline.cpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchPipeline.cpp) 的全线性编排分析

---

## 1. 全局管线总览

管线采用 **纯线性编排、无虚分派** 架构，所有算法模块均为 header-only（`.hpp`），作为 `TouchPipeline` 的成员变量直接持有。每帧调用 `Process()` 按以下 6 个阶段依次执行：

```mermaid
flowchart TB
    subgraph Input["📥 输入"]
        RAW["HeatmapFrame<br/>rawPtr (7B header + 4800B matrix + 256B suffix)"]
    end

    subgraph P1["Phase 1: 帧解析"]
        MFP["MasterFrameParser<br/>小端字节序 → int16 矩阵<br/>解析 Master/Slave Suffix"]
    end

    subgraph P2["Phase 2: 信号调理"]
        BL["BaselineSubtraction<br/>动态基线跟踪 + EMA 更新"]
        CMF["CMFProcessor<br/>共模滤波（行/列均值消除）"]
        GIIR["GridIIRProcessor<br/>时域门控IIR衰减 + 噪声地板截断"]
    end

    subgraph EarlyExit["⚡ 早退检查"]
        CHECK{"hasFinger<br/>||<br/>hasLiveTracks?"}
    end

    subgraph P3["Phase 3: 候选生成"]
        MZD["MacroZoneDetector<br/>BFS 8连通分量标记"]
        PD["PeakDetector<br/>局部极大值检测 + 多重滤波"]
    end

    subgraph P4["Phase 4: 分类与评估"]
        TC["TouchClassifier<br/>Zone 级 Palm/Finger 评分<br/>+ Peak 级二次评估"]
    end

    subgraph P5["Phase 5: 接触点提取与后处理"]
        CE["ContactExtractor<br/>ZoneExpander BFS 泛洪<br/>+ 加权质心计算<br/>+ 多指分割"]
        EC["EdgeCompensator<br/>边缘坐标补偿 (LUT)"]
        ER["EdgeRejector<br/>边缘误触抑制"]
        STS["StylusTouchSuppressor<br/>笔触局部抑制"]
    end

    subgraph P6["Phase 6: 跟踪、滤波与手势"]
        TT["TouchTracker<br/>Hungarian / 贪心最近邻匹配<br/>+ GapRelink + AFT 抑制"]
        CF["CoordinateFilter<br/>1-Euro 低通滤波"]
        GSM["TouchGestureStateMachine<br/>5 相手势生命周期<br/>Down → Drag → LongPress → Up"]
    end

    subgraph Output["📤 输出"]
        OUT["frame.contacts[]<br/>（含 id, x, y, state, reportEvent）"]
    end

    RAW --> MFP
    MFP --> BL
    BL --> CHECK
    CHECK -- "No" --> IDLE["ResetIdleOutputs()"]
    CHECK -- "Yes" --> CMF
    CMF --> GIIR
    GIIR --> MZD
    MZD --> PD
    PD --> TC
    TC --> CE
    CE --> EC
    EC --> ER
    ER --> STS
    STS --> TT
    TT --> CF
    CF --> GSM
    GSM --> OUT

    style P1 fill:#1e3a5f,stroke:#4a9eff,color:#fff
    style P2 fill:#2d1f4e,stroke:#8b5cf6,color:#fff
    style P3 fill:#1a3c34,stroke:#10b981,color:#fff
    style P4 fill:#3d2c1a,stroke:#f59e0b,color:#fff
    style P5 fill:#3a1a1a,stroke:#ef4444,color:#fff
    style P6 fill:#1a2d3a,stroke:#06b6d4,color:#fff
```

---

## 2. 各阶段详细分析

### Phase 1: 帧解析 — [MasterFrameParser](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/MasterFrameParser.hpp)

| 项目 | 说明 |
|------|------|
| **输入** | `frame.rawPtr`（5063B = 7B header + 4800B matrix + 256B suffix） |
| **输出** | `frame.heatmapMatrix[40][60]`（int16_t），`frame.masterSuffix`，`frame.slaveSuffix` |
| **算法** | 逐单元小端无对齐加载（`raw_ptr[i*2] \| raw_ptr[i*2+1]<<8`），MSVC O2 可自动向量化为 NEON |

---

### Phase 2: 信号调理

#### 2.1 [BaselineSubtraction](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/BaselineSubtraction.hpp) — 动态基线跟踪

```mermaid
flowchart LR
    subgraph PerCell["逐单元处理"]
        RAW["raw[i]"] --> DELTA["delta = raw - baseline"]
        DELTA --> COND{判断区间}
        COND -- "|delta| ≤ noiseDeadband" --> NOISE["噪声区<br/>微调基线 (αnoise)"]
        COND -- "delta ≥ touchFreeze" --> FREEZE["触摸冻结<br/>保持基线不变"]
        COND -- "delta > posDriftDB" --> POSDRIFT["正漂移<br/>缓慢上调 (αpos)"]
        COND -- "delta < -negDB" --> NEGDRIFT["负漂移<br/>快速下调 (αneg)"]
    end

    FREEZE --> HOLD["releaseHold 计时器<br/>延迟恢复基线更新"]
    NOISE --> OUT["output = |residual| ≤ deadband ? 0 : residual"]
    POSDRIFT --> OUT
    NEGDRIFT --> OUT
```

**核心机制：**
- **Q8 定点数基线**：`m_baselineQ8[i]` 使用 8 位小数精度，避免浮点运算
- **广域正偏移检测**：若 >12.5% 的单元超过 `touchFreezeThreshold`，视为传感器整体偏移，跳过冻结
- **EMA 更新**：`update = (delta << 8) >> alphaShift`，步长受 `maxStep` 钳制
- **采集模式**：初始帧使用 `acquisitionAlphaShift`（更大步长）快速收敛

#### 2.2 [CMFProcessor](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/CMFProcessor.hpp) — 共模滤波

| 维度 | 算法 |
|------|------|
| **行模式** | 每行排除 >`exclusionThreshold` 的单元 → 计算均值 → 全行减去均值 |
| **列模式** | 同理，按列维度 |
| **双维度** | 先行后列依次处理 |

- ARM64 使用 NEON SIMD 加速（`int16x8_t` 批量处理）
- `maxCorrection` 钳制最大校正量，防止过度补偿

#### 2.3 [GridIIRProcessor](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/GridIIRProcessor.hpp) — 时域门控 IIR

```mermaid
flowchart LR
    FRAMEMAX["frameMax"] --> DYN["dynThreshold = max(frameMax×gateRatio, staticFloor)"]
    DYN --> GATE{"signal ≥ dynThreshold<br/>or signal ≥ preserveThreshold?"}
    GATE -- "Yes" --> PASS["直通（保留原始信号）"]
    GATE -- "No" --> IIR["IIR 混合:<br/>val = (decayW × current + histW × history) / 256"]
    IIR --> DECAY["val = max(0, val - decayStep)"]
    DECAY --> FLOOR{"val < noiseFloorCutoff?"}
    FLOOR -- "Yes" --> ZERO["输出 0"]
    FLOOR -- "No" --> FILTERED["输出 filtered"]
```

**设计意图**：抑制低于动态阈值的残留信号尾巴，同时保护候选触摸区域的信号完整性。

---

### Phase 3: 候选生成

#### 3.1 [MacroZoneDetector](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/MacroZoneDetector.hpp) — 宏区域检测

| 项目 | 说明 |
|------|------|
| **算法** | BFS 8-连通分量标记 |
| **阈值** | 使用 PeakDetector 的 `m_threshold` |
| **输出** | `vector<MacroZone>`，每个含 `pixels[]`, `area`, `signalSum`, `bbox` |
| **优化** | 栈分配 BFS 队列（无堆分配），`visitEpoch` 纪元标记避免逐帧 memset |

```mermaid
flowchart LR
    HEAT["heatmapMatrix"] --> SCAN["逐单元扫描"]
    SCAN --> THR{"signal ≥ threshold?"}
    THR -- "Yes" --> BFS["BFS 8连通扩展"]
    BFS --> ZONE["记录 MacroZone:<br/>pixels, area, signalSum, bbox"]
    THR -- "No" --> SKIP["跳过"]
```

#### 3.2 [PeakDetector](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/PeakDetector.hpp) — 峰值检测

这是管线中最复杂的检测模块，包含 **7 步流水线**：

```mermaid
flowchart TB
    S1["① DetectInRange<br/>非对称局部极大值检测<br/>（下方/右方 ≤ ，上方/左方 <）"]
    S1b["① ClosePeakSaddleFilter<br/>鞍点分析去除紧邻假峰"]
    S2["② Z8Filter<br/>邻域信号总和过低 → 剔除孤立尖刺<br/>peak.z >> 5 > neighborSignalSum"]
    S3["③ Z1Filter<br/>signal < threshold → 剔除弱峰"]
    S3b["③½ MacroZoneMinArea<br/>所属宏区域面积 < 阈值 → 剔除"]
    S4["④ EdgePeakFilter<br/>边缘列最弱峰 < maxSig×5/8 → 剔除"]
    S5["⑤ SortPeaks<br/>按信号强度升序排列"]
    S6["⑥ Cap to maxPeaks<br/>截断上限（默认 20）"]
    S7["⑦ TrackPeakIDs<br/>贪心最近邻匹配（Manhattan ≤ 3）<br/>分配持久 ID + tzAge 计数"]

    S1 --> S1b --> S2 --> S3 --> S3b --> S4 --> S5 --> S6 --> S7

    style S1 fill:#1a3c34,stroke:#10b981,color:#fff
    style S7 fill:#1e3a5f,stroke:#4a9eff,color:#fff
```

**PressureDrift 检测**（步骤 ① 中）：

| 条件 | 描述 |
|------|------|
| `peakSig` 在 `[3/8, 3/4] × sigTholdLimit` 范围内 | 信号中等 |
| 行无尖锐梯度突变 | 无局部凸起 |
| `signalSum ≥ peakSig × 9/2` | 信号分布平坦 |
| `peakSig × 6 ≥ gradientSum` | 梯度变化不显著 |
| → 判定为掌压漂移伪峰，剔除 | |

---

### Phase 4: 候选分类 — [TouchClassifier](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchClassifier.hpp)

分类器执行 **双层评估**：Zone 级 + Peak 级。

#### 4.1 Zone 级特征分析

```mermaid
flowchart LR
    ZONE["MacroZone"] --> FEAT["BuildFeature()"]
    FEAT --> METRICS["计算特征:<br/>area, signalSum, density,<br/>aspectRatio, fillRatio,<br/>signalVariance, edgeTouchMask"]
    METRICS --> FLAGS["设置 PalmReasonFlags:<br/>LargeArea, LargeSignalSum,<br/>LowDensity, Elongated,<br/>HighFillRatio, EdgeWideContact,<br/>FlatSignalShape, StrongSharpPeak"]
    FLAGS --> SCORE["计算 palmScore / fingerScore"]
    SCORE --> CLASS{"palmClass"}
    CLASS -- "area≥likely & palm≥0.55" --> PL["PalmLikely"]
    CLASS -- "palm≥0.35" --> PC["PalmCandidate"]
    CLASS -- "finger≥0.55" --> FL["FingerLikely"]
    CLASS -- "else" --> AMB["Ambiguous"]
```

**Palm 评分权重表：**

| 条件 | 权重 |
|------|------|
| area ≥ areaThreshold (50) | +0.35 |
| area ≥ candidateArea (35) | +0.20 |
| signalSum ≥ signalSumThreshold (80000) | +0.25 |
| LowDensity | +0.15 |
| Elongated | +0.15 |
| HighFillRatio | +0.15 |
| EdgeWideContact | +0.10 |
| FlatSignalShape | +0.10 |
| **StrongSharpPeak（手指证据）** | **-0.20** |

#### 4.2 Peak 级评估

| 指标 | 计算方式 |
|------|----------|
| **prominence** | `peak.z - localMean5×5` |
| **sharpness** | `peak.z / localMean5×5` |
| **fingerScore** | prominence ≥ threshold (+0.45) + sharpness ≥ threshold (+0.35) + zone=FingerLikely (+0.20) |
| **palmScore** | zone.palmScore × 0.45 + flatPalmShape (+0.45) + inPalmZone & !strongFinger (+0.15) |

#### 4.3 Palm Shadow 机制

```mermaid
flowchart LR
    PL2["PalmLikely/PalmCandidate<br/>zone"] --> SEED["SeedPalmShadow<br/>膨胀半径 = palmShadowRadius"]
    SEED --> AGE["m_palmShadowAge[cell]<br/>= holdFrames (12帧)"]
    AGE --> DECAY["每帧衰减 age--"]
    DECAY --> APPLY["后续帧：zone 与 shadow 重叠<br/>→ 强制 PalmLikely"]
```

---

### Phase 5: 接触点提取与后处理

#### 5.1 [ContactExtractor](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/ContactExtractor.hpp) + [ZoneExpander](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/ZoneExpander.hpp)

```mermaid
flowchart TB
    PEAKS["Peaks[]"] --> ALLOW{"allowContact?<br/>(非 PalmLikely)"}
    ALLOW -- "No" --> SKIP["跳过（仅作证据）"]
    ALLOW -- "Yes" --> FLOOD["FloodFill BFS<br/>阈值 = min(sigThold, peakSig) × scale"]

    subgraph FloodFill["BFS 泛洪扩展"]
        BFS_CORE["核心区: signal ≥ zoneThold<br/>→ 累积加权质心"]
        BFS_EDGE["边缘区: 0 < signal < zoneThold<br/>→ 仅标记，不入质心"]
    end
    FLOOD --> FloodFill

    FloodFill --> DILATE["DilateErode<br/>形态学闭操作"]
    DILATE --> MARK["MarkEdges<br/>标记 zone 边界像素"]
    MARK --> ABSORB["ScanAbsorbedPeaks<br/>检测被其他 zone 吸收的峰"]
    ABSORB --> CENTROID["ComputeCentroids"]

    subgraph CentroidCalc["质心计算"]
        NF_SINGLE["NF/FF（单峰）:<br/>加权质心<br/>cx = Σ(col×128×sig) / Σ(sig)"]
        MF_MULTI["MF（多峰）:<br/>PartitionMultiFingerZone<br/>堆驱动 Voronoi 分割<br/>→ 每子区独立质心"]
    end
    CENTROID --> CentroidCalc

    CentroidCalc --> CONTACTS["frame.contacts[]"]
    CONTACTS --> SIZEEST["TouchSizeCalculator<br/>sizeMm = f(signalSum)"]
```

**Palm-Aware Expansion（手指在掌心中的特殊处理）：**
- 当 peak 被判为 `FingerLikely` 且 zone 为 `PalmCandidate/PalmLikely` 时
- 提高扩展阈值至 `peakSig × fingerInPalmThresholdRatio`（70%）
- 限制最大扩展半径为 `fingerInPalmMaxRadius`（3 格）
- 效果：在掌心检测到手指时，只扩展手指尖锐区域，不与掌域混合

#### 5.2 [EdgeCompensator](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/EdgeCompensation.hpp) — 边缘坐标补偿

```mermaid
flowchart LR
    TOUCH["TouchContact(x,y)"] --> NEAR{"靠近传感器边缘?"}
    NEAR -- "Yes" --> DIST["计算到边缘距离 (Q8)"]
    DIST --> LUT["查 g_ctd256Ln[256] LUT<br/>获取对数补偿偏移"]
    LUT --> BLEND["ECGetFinalOffset<br/>线性混合区插值"]
    BLEND --> CORRECTED["校正后坐标"]
    NEAR -- "No" --> PASS["保持原坐标"]
```

**LUT 原理**：使用 256 项对数表 `g_ctd256Ln[]` 实现非线性边缘补偿，模拟传感器边缘信号衰减曲线的逆映射。4 个边缘（上/下/左/右）各有独立的分段配置 `ECProfile`。

#### 5.3 [EdgeRejector](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/EdgeCompensation.hpp#L368-L404) — 边缘误触抑制

新触摸（`state == 0`）如果 EC 未能校正且仍贴在边缘（`dist ≤ edgeMargin`），则标记为不上报。

#### 5.4 [StylusTouchSuppressor](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/StylusTouchSuppressor.hpp) — 笔触局部抑制

```mermaid
flowchart LR
    PEN["Stylus 状态"] --> EVIDENCE["BuildStylusNoiseEvidence<br/>评估笔尖活跃度"]
    EVIDENCE --> LOCAL{"pointValid &<br/>active?"}
    LOCAL -- "Yes" --> RADIUS["检测 touch 在笔尖半径内"]
    RADIUS --> STRONG{"strongTouch?<br/>(signalSum≥6000 & area≥12)"}
    STRONG -- "Yes" --> KEEP["保留"]
    STRONG -- "No" --> WEAK{"weakTouch &<br/>overlap?"}
    WEAK -- "Yes" --> SUPPRESS["移除 contact"]
    WEAK -- "No" --> KEEP
```

---

### Phase 6: 跟踪、滤波与手势

#### 6.1 [TouchTracker](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchTracker.hpp) — 触摸跟踪器

这是管线中代码量最大（846 行）的模块：

```mermaid
flowchart TB
    subgraph Matching["匹配阶段"]
        ACTIVE["活跃轨道<br/>Active tracks"] --> HUNGARIAN["Hungarian 算法<br/>（或贪心最近邻）"]
        HUNGARIAN --> ALWAYS["AlwaysMatch<br/>距离 ≤ 2.2 的近距离强制匹配"]
        SILENT["SilentGap 轨道<br/>（丢失 ≤ gapWindow 帧）"] --> GAPRELINK["GapRelink<br/>双向最佳匹配 + 歧义校验"]
    end

    subgraph TrackUpdate["轨道更新"]
        MATCHED["匹配成功"] --> UPDATE["更新坐标、速度、年龄<br/>vx = (new.x - old.x) / span"]
        UNMATCHED_CUR["未匹配当前 contact"] --> NEW_TRACK["创建新轨道<br/>+ TouchDown Reject 检查<br/>+ Debounce 计算"]
        UNMATCHED_PRE["未匹配历史轨道"] --> GAP{"GapRelink<br/>enabled?"}
        GAP -- "≤ window" --> SILENT_KEEP["保持 SilentGap<br/>+ 预测坐标"]
        GAP -- "> window" --> LIFTOFF["生成 LiftOff 事件"]
    end

    subgraph PostTrack["后处理"]
        GHOST["GhostSuppressor<br/>RX 鬼影检测"]
        AFT["Stylus AFT<br/>Anti-Falsing Timer<br/>（笔尖附近弱触摸延迟抑制）"]
    end

    Matching --> TrackUpdate --> PostTrack
```

**关键参数：**

| 参数 | 默认值 | 作用 |
|------|--------|------|
| `maxTrackDistance` | 6.0 | 最大匹配搜索距离 |
| `alwaysMatchDistance` | 2.2 | 强制匹配距离（无需 gated） |
| `edgeTrackBoost` | 1.5× | 边缘区域匹配距离放大 |
| `accThresholdBoost` | 4.0× | 小触摸/边缘触摸的加速度门限放大 |
| `predictionScale` | 1.0 | 速度预测系数（v × scale） |
| `gapRelinkWindowFrames` | 2 | 间隙重连窗口 |
| `touchDownDebounceFrames` | 0 | 基础 TouchDown 去抖帧数 |

**TouchDown Reject 逻辑：**
- 弱信号（< 55）+ 极小面积（< 0.95mm）→ 拒绝
- 边缘 + 弱信号（< 90）→ 拒绝

#### 6.2 [CoordinateFilter](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/CoordinateFilter.hpp) — 1-Euro 低通滤波

$$\alpha = \frac{1}{1 + \tau \cdot rate}, \quad \tau = \frac{1}{2\pi \cdot cutoff}$$

$$cutoff = minCutoff + \beta \cdot |\dot{v}|$$

| 参数 | 默认值 | 效果 |
|------|--------|------|
| `minCutoff` | 5.0 | 静止时平滑强度（值越小越平滑） |
| `beta` | 0.05 | 速度自适应系数（值越大运动时延迟越低） |
| `dCutoff` | 1.0 | 速度估计的平滑截止频率 |

#### 6.3 [TouchGestureStateMachine](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchGestureStateMachine.hpp) — 手势状态机

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> PressCandidate : contact 出现
    PressCandidate --> Dragging : 位移 > dragThreshold
    PressCandidate --> LongPressHold : 静止帧数 ≥ longPressFrames\n且位移 ≤ tolerance
    PressCandidate --> ReleasePending : contact 丢失
    Dragging --> ReleasePending : contact 丢失
    LongPressHold --> Dragging : 位移 > dragThreshold
    LongPressHold --> ReleasePending : contact 丢失
    ReleasePending --> PressCandidate : contact 恢复\n(从 PressCandidate 来)
    ReleasePending --> Dragging : contact 恢复\n(从 Dragging 来)
    ReleasePending --> LongPressHold : contact 恢复\n(从 LongPressHold 来)
    ReleasePending --> Idle : missingFrames > releasePendingFrames\n(发送 Up 事件)
```

**报告事件映射：**

| 状态 | 报告 |
|------|------|
| `Idle` | 不报告 |
| `PressCandidate` (stableFrames ≥ N) | `TouchReportDown` |
| `PressCandidate` (stableFrames < N) | 不报告（消抖中） |
| `Dragging` | `TouchReportMove` |
| `LongPressHold` | `TouchReportMove`（坐标锁定在 anchor） |
| `ReleasePending → Idle` | `TouchReportUp` |

---

## 3. 关键数据结构

```mermaid
classDiagram
    class HeatmapFrame {
        +uint8_t* rawPtr
        +int16_t heatmapMatrix[40][60]
        +MasterSuffix masterSuffix
        +SlaveSuffix slaveSuffix
        +vector~TouchContact~ contacts
        +StylusState stylus
        +uint64_t timestamp
    }

    class Peak {
        +int r, c
        +int16_t z
        +int neighborSignalSum
        +uint8_t id
        +int tzAge
        +int macroZoneIndex
        +int macroZoneArea
    }

    class MacroZoneFeature {
        +int area, signalSum
        +float density, aspectRatio
        +float fillRatio
        +PalmClass palmClass
        +float palmScore, fingerScore
        +uint32_t reasonFlags
    }

    class PeakEvaluation {
        +PalmClass palmClass
        +float palmScore, fingerScore
        +bool allowContact
        +float prominence, sharpness
        +PalmClass zonePalmClass
    }

    class TouchContact {
        +int id
        +float x, y, sizeMm
        +int area, signalSum
        +int state
        +int reportEvent
        +bool isEdge, isReported
        +uint32_t edgeFlags, lifeFlags
    }

    HeatmapFrame --> TouchContact
    Peak --> PeakEvaluation
    MacroZoneFeature --> PeakEvaluation
```

---

## 4. 模块依赖关系

```mermaid
graph LR
    MFP["MasterFrameParser"] --> BL["BaselineSubtraction"]
    BL --> CMF["CMFProcessor"]
    CMF --> GIIR["GridIIRProcessor"]
    GIIR --> MZD["MacroZoneDetector"]
    MZD --> PD["PeakDetector"]
    PD --> TC["TouchClassifier"]
    MZD --> TC
    TC --> CE["ContactExtractor"]
    PD --> CE
    CE --> EC["EdgeCompensator"]
    CE --> ER["EdgeRejector"]
    EC --> STS["StylusTouchSuppressor"]
    ER --> STS
    STS --> TT["TouchTracker"]
    TT --> GS["GhostSuppressor"]
    TT --> CF["CoordinateFilter"]
    CF --> GSM["GestureStateMachine"]

    TC -.->|"palmAwareExpansion<br/>参数同步"| CE
    TT -.->|"stylus 参数同步"| STS
    PD -.->|"threshold"| GIIR
    PD -.->|"threshold"| MZD

    style MFP fill:#1e3a5f,color:#fff
    style BL fill:#2d1f4e,color:#fff
    style CMF fill:#2d1f4e,color:#fff
    style GIIR fill:#2d1f4e,color:#fff
    style MZD fill:#1a3c34,color:#fff
    style PD fill:#1a3c34,color:#fff
    style TC fill:#3d2c1a,color:#fff
    style CE fill:#3a1a1a,color:#fff
    style EC fill:#3a1a1a,color:#fff
    style ER fill:#3a1a1a,color:#fff
    style STS fill:#3a1a1a,color:#fff
    style TT fill:#1a2d3a,color:#fff
    style CF fill:#1a2d3a,color:#fff
    style GSM fill:#1a2d3a,color:#fff
    style GS fill:#1a2d3a,color:#fff
```

> [!NOTE]
> 虚线箭头表示配置参数的跨模块同步，而非运行时数据流。

---

## 5. 文件清单

| 文件 | 大小 | 阶段 | 职责 |
|------|------|------|------|
| [TouchPipeline.h](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchPipeline.h) | 4.3KB | 编排 | 管线声明、成员持有所有模块 |
| [TouchPipeline.cpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchPipeline.cpp) | 63KB | 编排 | Process() + Config Schema/Save/Load |
| [MasterFrameParser.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/MasterFrameParser.hpp) | 1.7KB | P1 | 帧解析 |
| [BaselineSubtraction.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/BaselineSubtraction.hpp) | 5.4KB | P2 | 动态基线 |
| [CMFProcessor.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/CMFProcessor.hpp) | 6.5KB | P2 | 共模滤波 |
| [GridIIRProcessor.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/GridIIRProcessor.hpp) | 7.9KB | P2 | 时域 IIR 衰减 |
| [MacroZoneDetector.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/MacroZoneDetector.hpp) | 5.0KB | P3 | BFS 连通分量 |
| [PeakDetector.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/PeakDetector.hpp) | 19KB | P3 | 峰值检测 |
| [MSType.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/MSType.hpp) | 1.8KB | 公共 | Peak / MacroZoneFeature / PeakEvaluation 结构体 |
| [TouchClassifier.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchClassifier.hpp) | 15KB | P4 | Palm/Finger 分类 |
| [ContactExtractor.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/ContactExtractor.hpp) | 5.5KB | P5 | 微区分割 + 外壳 |
| [ZoneExpander.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/ZoneExpander.hpp) | 31KB | P5 | BFS 泛洪 + 质心 + 多指分割 |
| [EdgeCompensation.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/EdgeCompensation.hpp) | 17KB | P5 | 边缘补偿 LUT + 拒绝器 |
| [StylusTouchSuppressor.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/StylusTouchSuppressor.hpp) | 7.3KB | P5 | 笔触抑制 |
| [GhostSuppressor.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/GhostSuppressor.hpp) | 3.7KB | P6 | RX 鬼影抑制 |
| [TouchTracker.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchTracker.hpp) | 36KB | P6 | 多触摸跟踪 |
| [CoordinateFilter.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/CoordinateFilter.hpp) | 3.2KB | P6 | 1-Euro 滤波 |
| [TouchGestureStateMachine.hpp](file:///d:/source/repos/EGoTouchRev-rebuild/EGoTouchService/Solvers/TouchSolver/TouchGestureStateMachine.hpp) | 11KB | P6 | 手势状态机 |
