# 手掌抑制重构计划 — 保留峰值的分层评估架构

> 最后更新：2026-05-25  
> 状态：设计计划，待实现  
> 目标模块：`EGoTouchService/Solvers/TouchSolver/`

---

## 1. 背景与问题

当前手掌抑制由 `PalmRejector` 在峰值检测前执行：

```text
MacroZoneDetector
  → PalmRejector
  → PeakDetector
  → MicroZoneSegmenter
  → ZoneExpander
  → TouchSize
  → EdgeRejector
  → TouchTracker
  → Gesture
```

该流程的问题是：

1. `PalmRejector` 直接删除整个 `MacroZone`，导致掌面区域内或掌边附近的真实指尖峰值也被提前丢弃。
2. 手掌判定只依赖面积、总信号、密度、长宽比等少量单帧规则，缺少峰值形态、局部尖锐度、时序稳定性和 contact 结果的联合判断。
3. 一旦掌面和手指形成同一连通域，当前架构没有机会在后续阶段区分“掌面低尖锐度峰”和“真实指尖尖峰”。
4. 手掌逻辑发生在 `ZoneExpander` 之前，无法利用扩张后的 contact area、sizeMm、edge info、absorbed peaks 等更可靠特征。

重构原则：**早期阶段只检测和标记，不删除数据；最终是否上报由后期 contact + tracker 阶段裁决。**

---

## 2. 目标

### 2.1 功能目标

- 保留手掌区域产生的峰值，供后续峰值评估、峰值合并和手掌识别使用。
- 不再在 `PeakDetector` 前删除整个 `MacroZone`。
- 对每个 MacroZone、Peak、Contact 建立统一的 palm/finger 评估信息。
- 支持在掌面大连通域中保留真实指尖峰，同时抑制掌面钝峰。
- 支持掌面多峰合并，避免掌面被输出成多个 touch。
- 使用 tracker 时序降低单帧误杀和闪断。

### 2.2 非目标

- 不在第一阶段引入机器学习模型。
- 不改变原始 heatmap 信号处理链路。
- 不重写 `TouchTracker` 的 ID 分配和 gesture 状态机。
- 不移除现有配置项；旧配置先兼容映射到新字段。

---

## 3. 新流水线

目标流水线：

```text
MasterFrameParser
  → BaselineSubtraction
  → CMFProcessor
  → GridIIRProcessor

  → MacroZoneDetector
  → PalmAnalyzer                 // 新：MacroZone 画像，只打标签，不删除
  → PeakDetector                  // 保留所有峰，包括手掌峰
  → PeakEvaluator                 // 新：峰值级 finger/palm/ambiguous 评分
  → PalmAwarePeakMerger           // 新：掌面多峰合并，保留指尖峰
  → MicroZoneSegmenter            // 可继续用于诊断和峰域可视化
  → ZoneExpander                  // 增加 palm-aware 扩张策略
  → EdgeCompensator
  → TouchSizeCalculator
  → EdgeRejector
  → ContactPalmClassifier         // 新：contact 级 palm/finger 复判
  → TouchTracker                  // 增加 palm hysteresis / suppressed track
  → CoordinateFilter
  → TouchGestureStateMachine
```

核心变化：

```text
旧：MacroZone → 删除掌区 → Peak
新：MacroZone → 标记掌区 → Peak → 评估/合并/限制扩张 → Contact → 时序裁决
```

---

## 4. 数据模型设计

### 4.1 Palm 分类枚举

建议新增公共枚举，可放在 `TouchFrameTypes.h` 或新的 palm-specific 头文件中。

```cpp
enum class PalmClass : uint8_t {
    Unknown = 0,
    FingerLikely,
    Ambiguous,
    PalmCandidate,
    PalmLikely
};
```

### 4.2 MacroZoneFeature

`PalmAnalyzer` 输出 MacroZone 画像，不修改 `MacroZoneDetector` 的原始结果。

```cpp
struct MacroZoneFeature {
    int zoneIndex = -1;

    int area = 0;
    int signalSum = 0;
    float density = 0.0f;

    int bboxW = 0;
    int bboxH = 0;
    int bboxArea = 0;
    float aspectRatio = 1.0f;
    float fillRatio = 0.0f;

    int maxSignal = 0;
    float meanSignal = 0.0f;
    float signalVariance = 0.0f;

    int edgeTouchMask = 0;
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    uint32_t reasonFlags = 0;
};
```

建议 reason flags：

```text
0x0001 LargeArea
0x0002 LargeSignalSum
0x0004 LowDensity
0x0008 Elongated
0x0010 HighFillRatio
0x0020 EdgeWideContact
0x0040 FlatSignalShape
0x0080 StrongSharpPeakPresent
```

### 4.3 Peak 扩展信息

当前 `Peak` 只有位置、信号、邻域和 MacroZone 面积。建议扩展为：

```cpp
struct Peak {
    int r = 0;
    int c = 0;
    int16_t z = 0;
    int neighborSignalSum = 0;
    uint8_t id = 0;
    int tzAge = 0;

    int macroZoneIndex = -1;
    int macroZoneArea = 0;
    int macroZoneSignalSum = 0;

    float localMean3x3 = 0.0f;
    float localMean5x5 = 0.0f;
    float prominence = 0.0f;
    float sharpness = 0.0f;

    PalmClass zonePalmClass = PalmClass::Unknown;
    PalmClass peakPalmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    uint32_t evalFlags = 0;
};
```

如果不希望第一阶段扩大 `Peak` 结构，也可以新增并行数组：

```cpp
struct PeakEvaluation {
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    bool allowContact = true;
    bool palmEvidenceOnly = false;
    int mergeTarget = -1;
};
```

### 4.4 Contact 扩展信息

建议给 `TouchContact` 增加 palm 评估字段：

```cpp
PalmClass palmClass = PalmClass::Unknown;
float palmScore = 0.0f;
float fingerScore = 0.0f;
uint32_t palmFlags = 0;
```

若要最小化 ABI/IPC 影响，第一阶段可先只使用 `debugFlags` 和 `isReported`，把结构字段扩展放到第二阶段。

---

## 5. 模块职责

### 5.1 `PalmAnalyzer`

替代当前 `PalmRejector` 的早期删除行为。

职责：

- 输入 `HeatmapFrame` 和 `MacroZone` 列表。
- 计算每个 MacroZone 的形状、密度、边缘、信号分布特征。
- 输出 `MacroZoneFeature` 列表。
- 不删除 MacroZone。
- 不改变后续峰值检测输入。

接口建议：

```cpp
class PalmAnalyzer {
public:
    bool m_enabled = true;

    void Process(const HeatmapFrame& frame,
                 const std::vector<MacroZone>& macroZones);

    const std::vector<MacroZoneFeature>& GetZoneFeatures() const;
};
```

初始判定策略：

```text
PalmCandidate:
  area >= areaCandidateThreshold
  或 signalSum >= signalCandidateThreshold
  或 bbox 大且 fillRatio 高

PalmLikely:
  area 大
  且 fillRatio 高或 density 低
  且没有强尖锐峰证据

FingerLikely:
  area 小
  或 bbox 小
  或密度/形态接近普通指尖
```

注意：MacroZone 层只能给粗分类，不能做最终抑制。

### 5.2 `PeakEvaluator`

职责：

- 保留所有 peak。
- 计算峰值局部形态。
- 根据 MacroZone 上下文和局部尖锐度给峰分类。
- 给后续合并和扩张阶段提供策略。

接口建议：

```cpp
class PeakEvaluator {
public:
    void Process(const HeatmapFrame& frame,
                 std::span<const Peak> peaks,
                 const std::vector<MacroZoneFeature>& zoneFeatures);

    std::span<const PeakEvaluation> GetEvaluations() const;
};
```

核心特征：

```text
localMean3x3
localMean5x5
prominence = peak.z - localMean5x5
sharpness = peak.z / max(1, localMean5x5)
neighborRatio = peak.z / max(1, neighborSignalSum)
zonePalmClass
zone area / density / fillRatio
peak distance to zone edge / bbox center
```

初始规则：

```text
FingerLikely:
  prominence 高
  且 sharpness 高
  且 peak.z 超过基本阈值

PalmLikely:
  所在 zone 是 PalmCandidate/PalmLikely
  且 prominence 低
  且 sharpness 低

Ambiguous:
  大掌区内有一定尖锐度但不足以确认 finger
```

### 5.3 `PalmAwarePeakMerger`

职责：

- 合并掌面上产生的多个低尖锐度峰。
- 避免掌面钝峰生成多个 contact。
- 保留真实指尖峰。

接口建议：

```cpp
class PalmAwarePeakMerger {
public:
    void Process(std::span<const Peak> peaks,
                 std::span<const PeakEvaluation> evaluations);

    std::span<const Peak> GetContactPeaks() const;
    std::span<const PalmBlob> GetPalmEvidence() const;
};
```

策略：

```text
FingerLikely:
  保留为 contact peak

PalmLikely:
  不进入 contact peaks
  合并为 PalmBlob evidence

Ambiguous:
  若靠近 FingerLikely，合并到 finger 的局部区域
  若处于 PalmLikely zone 且低尖锐度，合并到 PalmBlob
  否则保留给 contact 后处理和 tracker 决策
```

### 5.4 `ZoneExpander` palm-aware 策略

当前 `ZoneExpander` 对所有峰使用统一扩张规则。新策略需要根据 peak evaluation 调整。

建议新增扩张策略：

```cpp
enum class ZoneExpansionPolicy : uint8_t {
    Normal,
    TightFingerInPalm,
    PalmEvidenceOnly,
    Suppressed
};
```

对应行为：

```text
Normal:
  沿用现有 threshold 计算。

TightFingerInPalm:
  使用更高 zone threshold。
  可限制最大扩张半径。
  避免掌面背景进入指尖 contact。

PalmEvidenceOnly:
  不生成普通 TouchContact。
  只输出 PalmBlob 或 debug evidence。

Suppressed:
  完全不进入 ZoneExpander。
```

初始参数建议：

```text
Normal zone threshold: min(sigThold, peak.z) * 0.50
Finger-in-palm threshold: peak.z * 0.65 ~ 0.75
Finger-in-palm max radius: 2 ~ 3 grid cells
PalmLikely allowContact: false
```

### 5.5 `ContactPalmClassifier`

职责：

- 在 `ZoneExpander`、`EdgeCompensator`、`TouchSize` 后复判 contact。
- 使用真实 contact area、signalSum、sizeMm、edge info、源 peak 评估结果做最终单帧分类。
- 不直接破坏 tracker 状态；优先打标和设置报告抑制建议。

接口建议：

```cpp
class ContactPalmClassifier {
public:
    void Process(std::vector<TouchContact>& contacts,
                 std::span<const PeakEvaluation> evaluations,
                 const std::vector<ZoneEdgeInfo>& edgeInfos);
};
```

建议规则：

```text
PalmLikely contact:
  area 大
  或 sizeMm 大
  或来自 PalmLikely peak
  且 fingerScore 低

FingerLikely contact:
  来自 FingerLikely peak
  或 area/size 在正常范围
  或已有稳定 tracker 映射

Ambiguous contact:
  交给 TouchTracker 时序裁决
```

### 5.6 `TouchTracker` 时序裁决

`TouchTracker` 是最终报告层前最适合做 hysteresis 的模块。

新增状态建议：

```cpp
struct TrackState {
    ...
    int palmCandidateFrames = 0;
    int fingerEvidenceFrames = 0;
    int palmSuppressFrames = 0;
    PalmClass palmClass = PalmClass::Unknown;
};
```

策略：

```text
新出现 PalmLikely:
  不报告或延迟报告。

新出现 Ambiguous:
  等待 2~3 帧 finger evidence。

已稳定 Finger track:
  不因单帧 PalmLikely 立刻抑制。

连续 PalmLikely:
  进入 suppressed track，保持 ID 内部存在但不报告。

Palm → Finger:
  需要连续 strong finger evidence 才恢复上报。
```

---

## 6. 配置设计

旧配置继续保留：

```text
PalmEnabled
PalmAreaThreshold
PalmSignalSumThreshold
PalmDensityThresholdLow
PalmAreaMinForDensity
PalmElongatedEnabled
PalmElongatedMinArea
PalmElongatedAspectRatio
```

新增配置建议：

```text
PalmAnalyzerEnabled
PalmCandidateAreaThreshold
PalmCandidateSignalThreshold
PalmLikelyAreaThreshold
PalmFillRatioThreshold
PalmFlatSharpnessThreshold
PalmStrongPeakProminence

PeakEvalEnabled
PeakEvalFingerProminence
PeakEvalFingerSharpness
PeakEvalPalmSharpnessMax
PeakEvalAmbiguousMargin

PalmPeakMergeEnabled
PalmPeakMergeRadius
PalmPeakMergeSharpnessMax

PalmAwareExpansionEnabled
PalmFingerInPalmThresholdRatio
PalmFingerInPalmMaxRadius
PalmLikelyAllowContact

ContactPalmClassifierEnabled
ContactPalmAreaThreshold
ContactPalmSizeThresholdMm
ContactPalmScoreThreshold

PalmTrackerHysteresisEnabled
PalmSuppressConfirmFrames
PalmRecoverConfirmFrames
PalmAmbiguousDebounceFrames
```

配置迁移：

- `PalmEnabled` 映射到新 palm pipeline 总开关。
- 旧 area/signal/density/aspect 配置继续影响 `PalmAnalyzer` 初始 score。
- 第一阶段不删除旧 key，避免 UI 和配置文件兼容性问题。

---

## 7. 分阶段落地计划

### Phase 1：停止早删，建立 MacroZone 画像

目标：不改变后续模块接口太多，先消除“整块误删”。

改动：

1. 将 `PalmRejector` 重构为 `PalmAnalyzer` 或保留类名但改变行为。
2. `Process()` 不再 `erase` MacroZone。
3. 新增 `MacroZoneFeature` 计算和缓存。
4. `TouchPipeline` 中调用位置保持在 `MacroZoneDetector` 后、`PeakDetector` 前。
5. 增加 diagnostic 字段或日志，输出 zone palm score/reason。

验收：

- 大掌区 MacroZone 不再被提前删除。
- `PeakDetector` 能看到掌区内所有候选峰。
- 旧配置仍可保存/加载。

### Phase 2：Peak 评估与手掌峰保留

目标：对所有 peak 进行 finger/palm/ambiguous 分类。

改动：

1. 新增 `PeakEvaluator.hpp`。
2. 为 peak 或并行 evaluation 数组增加 palm/finger score。
3. 在 `PeakDetector` 后调用 `PeakEvaluator`。
4. diagnostic 暴露每个 peak 的分类和评分。

验收：

- 掌面钝峰被标记为 PalmLikely 或 Ambiguous。
- 掌区内尖锐指尖峰能被标记为 FingerLikely。
- 不因 palm 分类直接丢弃 peak。

### Phase 3：Palm-aware 峰合并

目标：掌面多峰不再生成多个 contact。

改动：

1. 新增 `PalmAwarePeakMerger.hpp`。
2. 生成 `contactPeaks` 和 `palmEvidence` 两类输出。
3. `ZoneExpander` 输入从原始 peaks 改为 contact peaks，或者增加 allowContact mask。
4. 保留 palm evidence 给后续 contact classifier / tracker。

验收：

- 掌面多个低尖锐度峰不会全部输出成 touch。
- FingerLikely peak 不被掌面合并吞掉。

### Phase 4：Palm-aware ZoneExpander

目标：指尖在掌区内可以保留，但扩张不吸入掌面背景。

改动：

1. `ZoneExpander` 接收每个 peak 的 expansion policy。
2. 对 `TightFingerInPalm` 使用更高阈值和可选半径限制。
3. `PalmEvidenceOnly` 不生成普通 contact。

验收：

- 指尖 contact 的 area/sizeMm 不被掌面显著放大。
- 掌区内真实指尖坐标保持稳定。

### Phase 5：Contact 级复判

目标：使用扩张结果进行更可靠的单帧 palm 判断。

改动：

1. 新增 `ContactPalmClassifier.hpp`。
2. 在 `TouchSize` 后、`TouchTracker` 前执行。
3. 输出 contact palm class / score / suppress suggestion。

验收：

- 明显手掌 contact 不上报。
- 已有清晰 finger evidence 的 contact 不被误杀。

### Phase 6：Tracker 时序融合

目标：减少闪断、误杀和掌面多峰闪烁。

改动：

1. `TouchTracker::TrackState` 增加 palm hysteresis 状态。
2. 对 PalmLikely / Ambiguous / FingerLikely 做确认帧数。
3. suppressed track 保持内部 ID，但不报告。

验收：

- 单帧 palm 误判不导致稳定手指立即 up。
- 掌面持续存在时不输出多个 touch。
- Palm → Finger 恢复需要连续强 finger evidence。

---

## 8. 推荐实现顺序

优先顺序：

```text
1. PalmRejector 停止删除 MacroZone
2. MacroZoneFeature + diagnostic
3. PeakEvaluator
4. PalmAwarePeakMerger
5. ZoneExpander policy
6. ContactPalmClassifier
7. Tracker hysteresis
```

原因：

- 第 1 步能立即解决最严重的“过早误删”。
- 第 2-3 步提供可观测数据，方便调参。
- 第 4-5 步控制掌面多峰和掌面背景污染。
- 第 6-7 步负责最终上报质量和时序稳定性。

---

## 9. 测试计划

### 9.1 单元测试

建议新增测试：

```text
PalmAnalyzerFeatureTest
PeakEvaluatorPalmClassificationTest
PalmAwarePeakMergerTest
ZoneExpanderPalmPolicyTest
ContactPalmClassifierTest
TouchTrackerPalmHysteresisTest
```

核心用例：

1. 小面积单指：应为 FingerLikely。
2. 大面积低尖锐度掌面：MacroZone 为 PalmLikely，peak 为 PalmLikely。
3. 大掌区内一个强尖峰：zone 为 PalmCandidate，强尖峰为 FingerLikely。
4. 掌面多个低峰：合并为 palm evidence，不生成多个 contacts。
5. 指尖贴近掌面：使用 tight expansion，contact area 不过度膨胀。
6. 稳定手指单帧变大：tracker 不立即抑制。
7. 连续掌面：tracker 持续 suppressed，不上报 touch。

### 9.2 诊断验证

在 EGOTOUCH_DIAG 下建议显示：

```text
MacroZone palmClass / palmScore / reasonFlags
Peak palmClass / fingerScore / palmScore / prominence / sharpness
Merged peak target
Expansion policy
Contact palmClass / palmScore
Tracker palm hysteresis state
```

### 9.3 实机调参流程

1. 关闭最终抑制，只观察分类结果。
2. 记录单指、多指、掌压、掌边指尖、握持边缘等场景。
3. 调整 zone-level palm candidate 阈值。
4. 调整 peak-level finger prominence/sharpness 阈值。
5. 开启 palm peak merge。
6. 开启 palm-aware expansion。
7. 最后开启 contact classifier 和 tracker hysteresis。

---

## 10. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 保留手掌峰导致峰数量变多 | 增加后续处理压力，可能超出 max peaks | PalmAwarePeakMerger 在 ZoneExpander 前压缩 PalmLikely peaks |
| 掌区内指尖仍被判为 palm | 真实触摸被抑制 | FingerLikely 规则优先看局部尖锐度和 prominence |
| 掌面背景污染指尖 contact | sizeMm/area 变大，tracker 误判 | TightFingerInPalm 扩张策略提高阈值并限制半径 |
| 单帧误判造成闪断 | 手指 up/down 抖动 | Tracker hysteresis 保留稳定 finger track |
| 配置项增加过多 | UI 调参复杂 | 第一阶段隐藏高级项，只暴露核心阈值 |

---

## 11. 第一轮建议默认值

这些值只作为初始实机调参起点：

```text
PalmCandidateAreaThreshold = 35
PalmLikelyAreaThreshold = 55
PalmCandidateSignalThreshold = 80000
PalmFillRatioThreshold = 0.45
PalmFlatSharpnessThreshold = 1.35
PalmStrongPeakProminence = 120

PeakEvalFingerProminence = 100
PeakEvalFingerSharpness = 1.45
PeakEvalPalmSharpnessMax = 1.25
PeakEvalAmbiguousMargin = 0.15

PalmPeakMergeRadius = 4
PalmPeakMergeSharpnessMax = 1.30

PalmFingerInPalmThresholdRatio = 0.70
PalmFingerInPalmMaxRadius = 3
PalmLikelyAllowContact = false

ContactPalmAreaThreshold = 24
ContactPalmSizeThresholdMm = 4.0
ContactPalmScoreThreshold = 0.70

PalmSuppressConfirmFrames = 2
PalmRecoverConfirmFrames = 2
PalmAmbiguousDebounceFrames = 3
```

---

## 12. 最小可行版本

如果要先快速落地一个 MVP，建议只做：

```text
PalmRejector 停止 erase
  + MacroZoneFeature
  + PeakEvaluator
  + PalmLikely peak 不进入 ZoneExpander
  + FingerLikely in PalmCandidate 使用 tight expansion
```

MVP 不需要立即改 tracker，也不需要完整 ContactPalmClassifier。这样能先验证核心假设：**保留手掌峰后，可以通过峰值形态和扩张策略区分掌面与指尖。**
