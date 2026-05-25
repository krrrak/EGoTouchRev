# TouchSolver 七层重构实施规范

> 最后更新：2026-05-25  
> 状态：后续实施主规范  
> 目标目录：`EGoTouchService/Solvers/TouchSolver/`  
> 相关文档：`docs/touch_palm_rejection_refactor_plan.md`、`docs/Finger_Touch_Algorithm_Flow.md`

---

## 1. 目标

本规范定义后续手指触摸解算重构的目标架构、模块边界、数据契约、迁移顺序和验收标准。

核心目标：

1. 将当前松散的模块流水线整理为清晰的七层架构。
2. 删除“早期拒绝/删除”式 palm suppression，改为“保留证据、后置分类、触点生成阶段裁决”。
3. 合并职责重复模块，尤其是 `PalmRejector` 与 `PeakEvaluator` 的 palm/finger 判断逻辑。
4. 收窄 `PeakDetector`、`TouchTracker` 等模块职责，避免检测、分类、后处理、跟踪逻辑互相污染。
5. 形成可测试、可调参、可诊断的数据流。

非目标：

1. 不在本轮引入机器学习模型。
2. 不改写原始 USB/AFE 帧格式。
3. 不一次性重写全部触摸跟踪和手势状态机。
4. 不破坏现有配置文件兼容性。
5. 不要求一次提交完成全部重构。

---

## 2. 总体架构

TouchSolver 后续按七层组织：

```text
1. Frame Input Layer
   原始帧解析层

2. Signal Processing Layer
   信号预处理层

3. Candidate Generation Layer
   候选生成层

4. Candidate Classification Layer
   候选分类层

5. Contact Extraction Layer
   触点提取层

6. Contact Post-Processing Layer
   触点后处理层

7. Temporal Tracking / Reporting Layer
   时序跟踪与上报层
```

目标数据流：

```text
Raw Frame
  ↓
HeatmapFrame
  ↓
Clean Heatmap
  ↓
TouchZone[] + TouchPeak[]
  ↓
TouchCandidateEvaluation[]
  ↓
RawTouchContact[]
  ↓
CorrectedContact[]
  ↓
TrackedContact[]
  ↓
TouchPacket / ReportEvent
```

---

## 3. 七层职责边界

### 3.1 Frame Input Layer：原始帧解析层

职责：

- 将原始帧字节解析为 `HeatmapFrame`。
- 填充 heatmap、suffix、timestamp、raw pointer 等底层字段。

当前模块：

```text
MasterFrameParser
```

禁止职责：

- 不做 palm/finger/noise 判断。
- 不生成触点。
- 不做坐标修正、滤波、跟踪。

---

### 3.2 Signal Processing Layer：信号预处理层

职责：

- 基线扣除。
- 共模滤波。
- 网格 IIR 平滑。
- 输出可用于候选生成的 clean heatmap。

当前模块：

```text
BaselineSubtraction
CMFProcessor
GridIIRProcessor
```

后续组织建议：

```text
SignalPreprocessor
  ├─ BaselineSubtraction
  ├─ CMFProcessor
  └─ GridIIRProcessor
```

禁止职责：

- 不检测 connected component。
- 不检测 peak。
- 不做 palm suppression。
- 不修改 contact。

---

### 3.3 Candidate Generation Layer：候选生成层

职责：

- 从 clean heatmap 生成候选区域和候选峰。
- 只回答“哪里有信号区域”和“哪里有局部峰值”。

当前模块：

```text
MacroZoneDetector
PeakDetector
```

目标模块：

```text
TouchCandidateBuilder
  ├─ TouchZoneDetector       // 当前 MacroZoneDetector
  └─ TouchPeakDetector       // 当前 PeakDetector
```

#### MacroZone / TouchZone 定义

`MacroZone` 是超过阈值的一片连续信号区域，即 connected component。

后续建议将概念统一为 `TouchZone`：

```cpp
struct TouchZone {
    int id;
    std::span<const int> pixels;
    int area;
    int signalSum;
    int minR;
    int maxR;
    int minC;
    int maxC;
};
```

迁移规则：

- 第一阶段可继续保留 `MacroZone` 类型名。
- 新代码和文档中优先使用 `TouchZone` 语义。
- `MacroZone` 只允许表示 connected component，不允许携带 palm/finger 最终结论。

#### PeakDetector 职责收窄

`PeakDetector` 只输出几何峰值：

```cpp
struct TouchPeak {
    int id;
    int row;
    int col;
    int signal;
    int zoneId;
    int neighborSignalSum;
};
```

禁止在 `PeakDetector` 中新增或维护：

- `palmScore`
- `fingerScore`
- `PalmClass`
- `allowContact`
- `evalFlags`
- contact suppression 策略

---

### 3.4 Candidate Classification Layer：候选分类层

职责：

- 统一做 zone-level 和 peak-level 的 palm/finger/noise/ambiguous 分类。
- 生成每个 peak 是否允许进入 contact extraction 的裁决。
- 保留 palm evidence，不删除原始 zone/peak。

当前模块合并来源：

```text
PalmRejector
PeakEvaluator
```

目标模块：

```text
TouchClassifier
  ├─ ZoneFeatureExtractor
  ├─ PeakFeatureExtractor
  ├─ ZoneClassifier
  └─ PeakClassifier
```

`PalmRejector` 后续处理规则：

1. 不再作为长期模块名保留。
2. 旧代码中的区域级特征计算迁入 `TouchClassifier`。
3. 旧 `PalmRejector::Process()` 的“删除/拒绝”语义彻底退役。
4. 配置兼容期内可以保留旧配置键，但实现归属应迁到 `TouchClassifier`。

分类输出建议：

```cpp
enum class TouchClass : uint8_t {
    Unknown = 0,
    FingerLikely,
    PalmLikely,
    NoiseLikely,
    Ambiguous
};

struct ZoneEvaluation {
    int zoneId = -1;
    TouchClass touchClass = TouchClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    uint32_t reasonFlags = 0;
};

struct PeakEvaluation {
    int peakId = -1;
    int zoneId = -1;
    TouchClass touchClass = TouchClass::Unknown;
    bool allowContact = true;
    bool evidenceOnly = false;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    float localMean3x3 = 0.0f;
    float localMean5x5 = 0.0f;
    float prominence = 0.0f;
    float sharpness = 0.0f;
    uint32_t reasonFlags = 0;
};
```

裁决原则：

```text
TouchClass::PalmLikely:
  默认 allowContact=false，保留为 evidenceOnly。

TouchClass::FingerLikely:
  allowContact=true。

TouchClass::Ambiguous:
  默认可由配置决定：保守模式不生成 contact，调试/兼容模式可生成 contact 并交给后处理/Tracker。

TouchClass::NoiseLikely:
  allowContact=false。
```

禁止职责：

- 不修改 heatmap。
- 不删除 zone/peak。
- 不生成 contact。
- 不做跨帧 tracking。

---

### 3.5 Contact Extraction Layer：触点提取层

职责：

- 从 `allowContact=true` 的 peak 生成 raw contact。
- 执行 flood fill / watershed / peak zone 分割。
- 计算几何信息：质心、面积、信号和、size、edge info。

当前模块合并来源：

```text
MicroZoneSegmenter
ZoneExpander
TouchSizeCalculator
```

目标模块：

```text
ContactExtractor
  ├─ PeakRegionSegmenter
  ├─ ContactFloodFiller
  ├─ ContactGeometryEstimator
  └─ ContactLimiter
```

输入契约：

```text
HeatmapFrame
TouchZone[]
TouchPeak[]
PeakEvaluation[]
```

输出契约：

```cpp
struct RawTouchContact {
    int sourcePeakId = -1;
    int sourceZoneId = -1;
    float x = 0.0f;
    float y = 0.0f;
    int area = 0;
    int signalSum = 0;
    float sizeMm = 0.0f;
    ZoneEdgeInfo edgeInfo{};
};
```

Contact extraction 策略：

```text
Normal finger:
  使用普通扩张阈值和半径。

Finger inside palm zone:
  使用 tighter threshold。
  限制最大扩张半径。
  避免吸入掌面背景。

PalmLikely / NoiseLikely:
  不生成普通 contact。
  可保留为诊断 evidence。
```

禁止职责：

- 不重新计算 palmScore/fingerScore。
- 不覆盖 classifier 的 `allowContact` 裁决。
- 不做跨帧 tracking。
- 不生成 Down/Move/Up 事件。

---

### 3.6 Contact Post-Processing Layer：触点后处理层

职责：

- 对 raw contact 做空间层面的修正和抑制。
- 处理边缘、鬼点、笔触摸互斥、硬件噪声等 contact-level 问题。

当前模块来源：

```text
EdgeCompensator
EdgeRejector
TouchTracker 中的 stylus suppression
TouchTracker 中的 RX ghost suppression
```

目标模块：

```text
ContactPostProcessor
  ├─ EdgeCompensator
  ├─ EdgeRejector
  ├─ GhostSuppressor
  └─ StylusTouchSuppressor
```

处理原则：

- 输入已经是 contact，不再处理 raw peak。
- 可以设置 contact 的 reported/suppressed 状态。
- 不负责 slot ID 和跨帧连续性。
- 不负责 Down/Move/Up 事件。

后续拆分规则：

1. 从 `TouchTracker` 中拆出 stylus suppression。
2. 从 `TouchTracker` 中拆出 RX ghost suppression。
3. `TouchTracker` 只接收经过 post-processing 的 contact。

---

### 3.7 Temporal Tracking / Reporting Layer：时序跟踪与上报层

职责：

- 对 contact 做跨帧 slot 匹配。
- 维护 tracking ID。
- 做 debounce、lift-off hold、relink、速度预测。
- 坐标滤波。
- 生成 Down/Move/Up 上报事件。

当前模块：

```text
TouchTracker
CoordinateFilter
TouchGestureStateMachine
```

目标组织：

```text
TemporalTouchPipeline
  ├─ TouchTracker
  ├─ CoordinateFilter
  └─ TouchGestureStateMachine
```

`TouchTracker` 职责收窄后只保留：

- slot matching
- track lifecycle
- debounce
- lift-off / relink
- prediction if required

禁止职责：

- 不做 palm/finger 单帧分类。
- 不做 stylus suppression。
- 不做 RX ghost suppression。
- 不重新估算 contact size。

---

## 4. 模块迁移映射

| 当前模块 | 目标归属 | 处理方式 |
| --- | --- | --- |
| `MasterFrameParser` | Frame Input Layer | 保留 |
| `BaselineSubtraction` | Signal Processing Layer | 保留，后续可由 `SignalPreprocessor` 编排 |
| `CMFProcessor` | Signal Processing Layer | 保留 |
| `GridIIRProcessor` | Signal Processing Layer | 保留 |
| `MacroZoneDetector` | Candidate Generation Layer | 保留，语义收窄为 connected component |
| `PeakDetector` | Candidate Generation Layer | 保留，删除分类字段 |
| `PalmRejector` | Candidate Classification Layer | 模块名退役，逻辑迁入 `TouchClassifier` |
| `PeakEvaluator` | Candidate Classification Layer | 合并进 `TouchClassifier` |
| `MicroZoneSegmenter` | Contact Extraction Layer | 并入 `ContactExtractor` 或保留为子模块 |
| `ZoneExpander` | Contact Extraction Layer | 重命名/重构为 contact flood fill 子模块 |
| `TouchSizeCalculator` | Contact Extraction Layer | 并入 `ContactGeometryEstimator` |
| `EdgeCompensator` | Contact Post-Processing Layer | 保留 |
| `EdgeRejector` | Contact Post-Processing Layer | 保留或并入 edge processor |
| Stylus suppression | Contact Post-Processing Layer | 从 `TouchTracker` 拆出 |
| RX ghost suppression | Contact Post-Processing Layer | 从 `TouchTracker` 拆出 |
| `TouchTracker` | Temporal Tracking Layer | 收窄为时序跟踪 |
| `CoordinateFilter` | Temporal Tracking Layer | 保留 |
| `TouchGestureStateMachine` | Temporal Tracking Layer | 保留 |

---

## 5. 数据所有权与稳定性规则

### 5.1 Index 稳定性

在同一帧内必须保证：

```text
zoneId 稳定指向 TouchZone[]
peakId 稳定指向 TouchPeak[]
evaluation.peakId 稳定指向 TouchPeak[]
contact.sourcePeakId 稳定指向 TouchPeak[]
contact.sourceZoneId 稳定指向 TouchZone[]
```

禁止在中途删除导致 index 漂移：

```text
禁止：erase zones
禁止：erase peaks 后继续使用旧 peakId
禁止：排序 contacts 后丢失 sourcePeakId/sourceZoneId
```

如需过滤，应使用：

```text
allowContact
isSuppressed
isReported
evidenceOnly
```

### 5.2 Heatmap 写入规则

- Signal Processing Layer 可以修改 heatmap。
- Candidate Generation 之后的模块默认不得修改 heatmap。
- 若必须修改，应使用独立 scratch buffer 或明确命名的 debug/output buffer。

### 5.3 Classification 不删除原则

分类阶段只允许：

```text
打分
打标签
设置 allowContact/evidenceOnly
记录 reasonFlags
```

分类阶段禁止：

```text
删除 MacroZone/TouchZone
删除 Peak
修改 heatmap
生成 contact
```

### 5.4 Contact 生成门控

唯一允许从 peak 到 contact 的门控字段：

```text
PeakEvaluation.allowContact
```

如果 contact 被抑制，应保留原因：

```text
TouchClass
reasonFlags
sourcePeakId
sourceZoneId
```

---

## 6. 配置规范

### 6.1 命名原则

新配置按目标模块命名：

```text
TouchClassifier*
ContactExtractor*
ContactPostProcessor*
TouchTracker*
```

旧 palm 配置键继续兼容，但不再作为新代码命名依据。

### 6.2 旧键兼容

兼容保留：

```text
PalmEnabled
PalmAreaThreshold
PalmSignalSumThreshold
PalmDensityThresholdLow
PalmAreaMinForDensity
PalmElongatedEnabled
PalmElongatedMinArea
PalmElongatedAspectRatio
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
PalmAwareExpansionEnabled
PalmFingerInPalmThresholdRatio
PalmFingerInPalmMaxRadius
PalmLikelyAllowContact
```

### 6.3 新键建议

新架构收口后建议新增或迁移到：

```text
TouchClassifierEnabled
TouchClassifierPalmCandidateAreaThreshold
TouchClassifierPalmLikelyAreaThreshold
TouchClassifierPalmSignalThreshold
TouchClassifierPalmFlatSharpnessMax
TouchClassifierFingerProminenceMin
TouchClassifierFingerSharpnessMin
TouchClassifierAmbiguousMargin

ContactExtractorPalmAwareEnabled
ContactExtractorFingerInPalmThresholdRatio
ContactExtractorFingerInPalmMaxRadius
ContactExtractorMaxContacts

ContactPostProcessorEdgeRejectEnabled
ContactPostProcessorGhostSuppressEnabled
ContactPostProcessorStylusSuppressEnabled

TouchTrackerDebounceFrames
TouchTrackerLiftHoldFrames
TouchTrackerRelinkFrames
```

迁移规则：

1. 第一阶段继续保存旧键。
2. 第二阶段新旧键同时读取，新键优先。
3. 第三阶段 UI 可显示新键，旧键仅作为兼容输入。
4. 不在同一改动中删除旧键和重构核心逻辑。

---

## 7. 分阶段实施计划

### Phase 0：固化当前 MVP 行为

目标：在正式重排前保证现有 palm MVP 行为有测试保护。

内容：

1. 保留当前“掌区不早删、PeakEvaluator 后置判定、ZoneExpander 跳过 PalmLikely”的行为。
2. 补齐针对典型 palm DVR 的回放/分析测试或脚本。
3. 确认旧配置 round-trip 不破坏。

验收：

- 典型掌压 DVR 不输出普通 touch。
- 掌区尖峰 finger 用例仍允许 contact。
- `TouchPalmRejectionMvpTest` 通过。
- `TouchPipelineConfigRoundTripTest` 通过。

---

### Phase 1：建立新数据契约

目标：先整理类型和 index 关系，不大改算法。

内容：

1. 新增或整理 `TouchZone` / `TouchPeak` / `ZoneEvaluation` / `PeakEvaluation` 类型。
2. 从 `Peak` 中移除或停止使用 palm/finger 分类字段。
3. 所有后续模块通过 `peakId` / `zoneId` 关联数据。
4. 保证 `MacroZone` 仅表示 connected component。

验收：

- `PeakDetector` 输出不包含分类职责。
- `PeakEvaluation` 是唯一 peak-level 分类输出。
- 所有测试通过。

---

### Phase 2：合并 PalmRejector + PeakEvaluator

目标：退役 `PalmRejector` 模块名，建立 `TouchClassifier`。

内容：

1. 创建 `TouchClassifier`。
2. 将 `PalmRejector` 的 zone feature/scoring 迁入 `TouchClassifier`。
3. 将 `PeakEvaluator` 的 peak feature/scoring 迁入 `TouchClassifier`。
4. `TouchPipeline` 中用 `TouchClassifier` 替代二者的编排。
5. 保留旧配置读取，并映射到 `TouchClassifier` 参数。

验收：

- `PalmRejector` 不再出现在主 pipeline 编排中。
- 区域级和峰值级 palm/finger 判定来自同一模块。
- palm DVR 与单指/多指回归通过。

---

### Phase 3：重构 ContactExtractor

目标：把触点生成逻辑从 `ZoneExpander` 扩展为明确的 contact extraction 层。

内容：

1. 创建 `ContactExtractor` 作为门面。
2. 将 `ZoneExpander` flood fill 逻辑迁为子模块或内部实现。
3. 将 `TouchSizeCalculator` 逻辑迁入 `ContactGeometryEstimator`。
4. 保证 `ContactExtractor` 只消费 `allowContact=true` 的 peak。
5. 输出包含 source peak/zone 和 edge info 的 raw contact。

验收：

- `TouchSizeCalculator` 不再与 `TouchTracker` 重复估算 size。
- palm-aware finger-in-palm 扩张仍有效。
- contacts 与 edge info 一一对应。

---

### Phase 4：拆分 ContactPostProcessor

目标：把空间后处理从 tracker 中剥离。

内容：

1. 创建 `ContactPostProcessor` 编排层。
2. 保留 `EdgeCompensator`、`EdgeRejector`。
3. 从 `TouchTracker` 拆出 `StylusTouchSuppressor`。
4. 从 `TouchTracker` 拆出 `GhostSuppressor`。
5. `TouchTracker` 输入变为已经空间后处理过的 contacts。

验收：

- `TouchTracker` 不再直接依赖 stylus suppression 细节。
- RX ghost 抑制有独立测试。
- stylus/touch 互斥有独立测试。

---

### Phase 5：收窄 TouchTracker

目标：让 tracker 只做时序跟踪。

内容：

1. 删除 tracker 内 contact size fallback/override。
2. 保留 slot matching、debounce、lift hold、relink。
3. 明确 `CoordinateFilter` 在 tracker 之后执行。
4. 明确 `TouchGestureStateMachine` 是唯一 report event owner。

验收：

- tracker 不覆盖 contact geometry。
- 稳定手指不因单帧分类波动闪断。
- Down/Move/Up 状态保持一致。

---

### Phase 6：命名和旧接口清理

目标：完成新架构命名收口。

内容：

1. 将主 pipeline 注释和阶段名改为七层架构。
2. 删除不再使用的旧类或变成兼容 wrapper。
3. 清理死字段、重复配置、重复 size 计算。
4. 更新测试名和文档。

验收：

- 主 pipeline 能直接读出七层流程。
- 无未使用 palm/peak 字段。
- 无重复 palm/finger 打分路径。

---

## 8. 测试规范

### 8.1 单元测试

必须覆盖：

```text
TouchClassifierZoneTest
TouchClassifierPeakTest
ContactExtractorPalmPolicyTest
ContactExtractorGeometryTest
ContactPostProcessorEdgeTest
ContactPostProcessorStylusSuppressTest
GhostSuppressorTest
TouchTrackerLifecycleTest
```

核心用例：

1. 小面积单指：zone/peak 均为 FingerLikely，生成 contact。
2. 大面积掌压：zone/peak 为 PalmLikely，不生成普通 contact。
3. 掌区尖峰：zone 可为 PalmLikely/PalmCandidate，但尖峰为 FingerLikely，生成 tight contact。
4. 多峰掌压：多个 palm peaks 不生成多个 contacts。
5. 边缘误触：post processor 抑制新触点。
6. stylus 附近噪声：由 `StylusTouchSuppressor` 抑制，不进入 tracker。
7. 稳定手指单帧异常：tracker 不应立即上报 Up。

### 8.2 DVR 回放测试

至少维护三类样本：

```text
Palm only DVR
Finger only DVR
Finger near/in palm DVR
```

每类样本应输出：

```text
frames
zone class counts
peak class counts
allowed peak count
suppressed peak count
contact count distribution
track count distribution
```

验收方向：

- Palm only：allowed peaks 接近 0，reported contacts 为 0。
- Finger only：reported contacts 稳定，无明显闪断。
- Finger in palm：真实 finger 可报告，contact area/size 不被掌面显著放大。

### 8.3 配置测试

必须保持：

- 旧 key 可读取。
- 旧 key 可保存。
- 新 key 若引入，读取优先级明确。
- round-trip 不丢失 palm/touch classifier 关键参数。

---

## 9. 诊断规范

在调试输出或 DVR 分析工具中，至少暴露：

```text
zoneId
zone area / signalSum / bbox / fillRatio
zone touchClass / palmScore / fingerScore / reasonFlags
peakId
peak row / col / signal
peak localMean / prominence / sharpness
peak touchClass / allowContact / evidenceOnly
contact sourcePeakId / sourceZoneId
contact x / y / area / signalSum / sizeMm
post-process suppression reason
track id / slot / lifecycle state
report event
```

诊断原则：

- 每个被抑制的 peak/contact 必须能解释原因。
- 每个 reported contact 必须能追溯到 source peak 和 source zone。
- 不允许出现“静默删除”候选的路径。

---

## 10. 代码实施规则

1. 新模块优先使用清晰职责名，不使用 `Rejector` 表达分类模块。
2. `Rejector` 只允许用于真正的后处理拒绝器，例如 edge reject。
3. 检测模块不分类。
4. 分类模块不删除。
5. 提取模块不重新分类。
6. 后处理模块不跟踪。
7. 跟踪模块不重新估算几何。
8. 任何跨数组引用必须使用稳定 id/index。
9. 任何 top-N 截断必须同步保留 contact、edge info、source id 的对应关系。
10. 每个阶段新增行为必须有单元测试或 DVR 回放验证。

---

## 11. 推荐提交顺序

```text
1. 文档与类型契约
2. Peak 结构收窄
3. TouchClassifier 新增并兼容旧 palm 参数
4. Pipeline 从 PalmRejector/PeakEvaluator 切到 TouchClassifier
5. ContactExtractor façade 包住现有 ZoneExpander
6. TouchSizeCalculator 并入 ContactExtractor
7. ContactPostProcessor 新增
8. Stylus/Ghost suppression 从 Tracker 拆出
9. Tracker 职责清理
10. 旧配置和旧 wrapper 收尾
```

每一步都应保持：

```text
可编译
目标单元测试通过
已有 palm DVR 行为不回退
```

---

## 12. 最终验收标准

完成重构后应满足：

1. `TouchPipeline` 主流程可清晰映射到七层架构。
2. `PalmRejector` 不再作为主流程模块存在。
3. `PeakDetector` 只输出候选峰。
4. palm/finger/noise/ambiguous 判定由 `TouchClassifier` 统一负责。
5. `ContactExtractor` 是唯一从 peak 生成 contact 的模块。
6. `TouchTracker` 不再包含 stylus suppression、ghost suppression、contact size 估算。
7. 每个 contact 可追溯到 source peak 和 source zone。
8. palm-only 样本不输出 touch。
9. finger-only 样本稳定输出 touch。
10. finger-in-palm 样本能保留真实指尖并限制掌面扩张。
11. 旧配置文件可继续加载。
12. 测试覆盖主要单指、多指、掌压、掌边指尖、边缘误触、stylus 干扰场景。

---

## 13. 与旧 palm 重构计划的关系

`docs/touch_palm_rejection_refactor_plan.md` 是 palm suppression 的阶段性计划，重点是解决“掌区早删”和“掌峰保留”。

本文档是后续 TouchSolver 总架构实施规范，范围更大：

```text
旧 palm 计划：
  解决 PalmRejector/PeakEvaluator/ZoneExpander 的 palm suppression 问题。

本文档：
  定义整个 TouchSolver 的七层结构、模块合并、数据契约和迁移路线。
```

后续涉及 TouchSolver 主流程重构时，以本文档为主规范；旧 palm 计划作为 palm suppression 背景和 MVP 记录保留。
