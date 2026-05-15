# NoPressInk 与 ASA_HPP3TX1GridDataProcesss 分析

> 基于 `TSACore.dll` 的 Ghidra 反编译与汇编核对结果。
> 相关函数地址：
>
> - `ASA_HPP3TX1GridDataProcesss @ 0x6bac668d`
> - `ASA_HPP3Process @ 0x6bac69c2`
> - `NoPressInkProcess @ 0x6bab6996`
> - `NoPressInkHandle @ 0x6bab6814`
>
> 关键调用链：
>
> `HPP3_DataProcess -> ASA_HPP3TX1GridDataProcesss -> ASA_HPP3Process -> NoPressInkProcess -> HPP3_PostPressureProcess`

## 总结

`NoPressInkProcess` 的本质不是直接计算压力，而是一个“**无真实压力时维持书写状态**”的门控与学习模块。

它做三件事：

1. 检查 NoPressInk 功能开关和 learned-table 状态。
2. 调 `NoPressInkHandle()` 根据当前信号决定是否进入/退出 no-press ink 状态。
3. 在背景中维护按位置和 tilt 自学习的 no-press 阈值表。

对应地，`ASA_HPP3TX1GridDataProcesss` 的作用是：

1. 从 TX1/TX2 grid 数据中提取最强 peak。
2. 计算坐标、tilt、噪声有效性。
3. 把后续 `NoPressInkProcess` 会读取的 `g_asaPrpt` 信号字段整理好。

其中最关键的一点是：**NoPressInk 读到的是 `ASA_HPP3TX1GridDataProcesss` 末尾重新整理后的 `wTx1Signal* / wTx2Signal*`，不是中间过程的临时值。**

## 1. NoPressInk 主流程

### 1.1 `NoPressInkProcess`

主流程如下：

1. 调 `ASA_IsHpp3NoPressInkFeatureEnabled()` 检查功能总开关。
2. 如果开启：
   - 若 `ASA_IsHpp3NoPressTLearnedFeatureEnabled() == 0`，或者 `g_noPressPara.bLearnedTableReady != 0`，则执行 `NoPressInkHandle()`。
   - 否则直接清 `g_asaStatic.bHadPressurePrev = 0`。
3. 如果 NoPressT learned 功能开启：
   - `bLearnedTableReady == 0` 时执行 `NoPressInkLearningPrepareProcess()`；
   - 否则执行 `NoPressInkLearningProcess()`。
4. 最后更新：

```c
bLastHpp3NoPressInkPressureActive = bHasPressure | bHadPressurePrev;
```

这个值只用于记录 no-press ink 压力活跃状态的变化。

### 1.2 `NoPressInkHandle`

`NoPressInkHandle()` 是核心门控函数，负责：

1. 缓存最近 20 帧的 `TX1/TX2/innerX/innerY/btPress` 历史。
2. 检测异常信号，设置 `dwNoPressInkAbnormalFlags`。
3. 若 tilt learned 功能未开启，或 `IsTiltLearnedOK()` 为真：
   - 更新 tilt compensation；
   - 更新 enter/exit threshold；
   - 根据 enter/exit 条件与 2 帧 debounce 更新 `bHadPressurePrev`。
4. 若 coordinate revise 功能开启但 `flagTX2Start == 0`，则强制清 `bHadPressurePrev`。

### 1.3 与最终压力输出的关系

`NoPressInkProcess` 之后会进入 `HPP3_PostPressureProcess()`。

其中有一段逻辑：

```c
if (bHasPressure == 0 && bHadPressurePrev != 0) {
    curPressure = prevPressure != 0 ? prevPressure : 10;
}
```

这说明 NoPressInk 的直接效果不是把 `bHasPressure` 重新置 1，而是让“当前真实压力已经没有了，但信号仍像在写字”的帧继续保留压力输出，避免笔迹中断。

## 2. NoPressInk 关键子流程

### 2.1 进入/退出判定

#### `EnterToNoPressInk`

- 当 `dwNoPressInkSignalCompareMode == 0` 时：

```c
wTx1Signal > (EnterTholdX / 2 + EnterTholdY / 2)
```

- 当 `dwNoPressInkSignalCompareMode != 0` 时：

```c
wTx1SignalX > EnterTholdX && wTx1SignalY > EnterTholdY
```

#### `ExitToNoPressInk`

- 当 `dwNoPressInkSignalCompareMode == 0` 时：

```c
wTx1Signal < (ExitTholdX / 2 + ExitTholdY / 2)
```

- 当 `dwNoPressInkSignalCompareMode != 0` 时：

```c
wTx1SignalX < ExitTholdX && wTx1SignalY < ExitTholdY
```

### 2.2 阈值更新

#### `UpdateNoPressInkThold`

1. 先调用 `GetNopressInkTholdFromLearnedTable(innerX, innerY)` 获取当前位置基础阈值。
2. 再加上 `GetNoPressInkTiltCompensation()` 的 tilt 补偿。
3. 最后乘以 `bNoPressInkEnterScale / bNoPressInkExitScale` 得到 enter/exit threshold。

#### `GetNopressInkTholdFromLearnedTable`

- 先把 `innerX >> 10`、`innerY >> 10` 映射到 TX/RX grid。
- 在当前位置周围 3x3 邻域内统计非零 learned table 项并做平均。
- 若邻域没有有效格点：
  - 优先回退到 `wMaxSignalInTable`；
  - 否则回退到 `wDefaultNoPressInkThold`。

#### `GetNoPressInkTiltCompensation`

- 对 `wTx2Signal` 做区间裁剪：`[wTx2CompSignalMin, wTx2CompSignalMax]`。
- 减去最小值后，用 `wActiveTx2CompCoef` 线性换算成补偿值。

### 2.3 学习流程

#### `NoPressInkLearningPrepareProcess`

learned table 还没 ready 时执行：

- 统计 pressure down 次数。
- 统计 prepare 周期内最大补偿后 TX1 信号。
- 统计 prepare 帧数。
- 条件满足时：
  - `wPrepareDownCount > 1`
  - `wPreparePeriodCount > 100`
  - `dwNoPressInkAbnormalFlags == 0`
- 则把 `bLearnedTableReady = 1`，并把 safe table 按当前观测到的最大信号缩放成工作 table。

#### `NoPressInkLearningProcess`

learned table ready 后执行：

- 新一段 pressure 开始时清空 short-term table。
- pressure 保持期间：
  - `IsMeetNoPressInkLearningCondition()` 要求连续稳定约 30 帧；
  - 同时 tilt 条件成立、`btPressBuf[3] <= 4000`、且没有 abnormal flag；
  - 满足后调 `CalcNoPressInkThd()` 写 short-term table。
- pressure 结束时：
  - `CheckShortTermTable()`
  - `UpdateLongTermTable()`
  - `UpdateTiltCompScal()`
  - `UpdateSafeTable()`

#### `CalcNoPressInkThd`

1. 从历史窗口 `[10, 20)` 取均值样本。
2. 用 `GetCompensationByTilt(wTx2Signal)` 扣除 tilt 补偿。
3. 用 `innerX >> 10`、`innerY >> 10` 找 grid cell。
4. 当前信号若低于：
   - short-term max 的 80%，或
   - long-term table max 的 70%，
   则拒绝本次学习。
5. 否则更新当前 grid 的 short-term threshold 和对应的 TX2 信号。

### 2.4 异常检测

`CheckSignalAbnormalStatus()` 会设置 `dwNoPressInkAbnormalFlags`，主要包括：

- `0x2`：单帧信号跳变过大。
- `0x4`：10 帧累计跳变过大。
- `0x8`：palm / coupling noise 过大。
- `0x10`：当前样本与已学 tilt offset 模型明显不一致。

这些标志会直接阻断 no-press ink 学习，部分情况下也会让 `bHadPressurePrev` 被清掉。

## 3. `ASA_HPP3TX1GridDataProcesss` 输出给 NoPressInk 的变量

`ASA_HPP3TX1GridDataProcesss` 的主要阶段如下：

1. `GetGridTx1Peaks()`：提取 TX1 grid 最强 peak，并得到其 3x3 sum。
2. `TX1LinePeaksProcess()`：更新 line peak 相关结果，供噪声和边缘判定使用。
3. `HPP3_NoiseProcess()`：筛 peak、更新 bypass 状态。
4. `TX1CoordinateProcess()`：求 `innerX/innerY`。
5. 若 TX2 存在：
   - `GetGridTx2Peaks()`：提取 TX2 grid 最强 peak，并得到其 3x3 sum；
   - `TiltProcess()`：计算 tilt、置 `flagTX2Start`。
6. 末尾把 `wTx1Signal* / wTx2Signal*` 字段整理成后续模块统一读取的格式。

### 3.1 最关键结论

在 **grid 模式** 下，`wTx1SignalX / wTx1SignalY / wTx1Signal` 不再表示真正的 X/Y 分轴线信号，而是都被回填成 **TX1 strongest grid peak 的 3x3 sum**。

更重要的是，汇编核对表明：

- `wTx2SignalX = tx2PeakTable.wSelectedPeak3x3Sum`
- `wTx2SignalY = wTx1SignalX`
- `wTx2Signal = wTx1SignalX`

也就是说：

- `TiltProcess()` 之前，`wTx2Signal` 确实先被写成了 **真 TX2 3x3 sum**；
- 但函数结束前又被重新整理；
- **NoPressInk 最终读到的是整理后的值。**

这不是反编译误差，已经由 `0x6bac673e ~ 0x6bac67d9` 的汇编写入顺序确认。

## 4. NoPressInk 直接使用的字段表

| 字段 | 在 grid 路径中的来源 | 实际含义 | NoPressInk 中的用途 |
| --- | --- | --- | --- |
| `g_asaPrpt.tx1PeakTable.wSelectedPeak3x3Sum` | `GetGridTx1Peaks()` | TX1 strongest peak 的 3x3 区域总能量 | 后续被回填到 `wTx1SignalX/Y/Signal` |
| `g_asaPrpt.wTx1SignalX` | 函数末尾写入 `tx1PeakTable.wSelectedPeak3x3Sum` | grid 模式下的 TX1 主信号标量 | `dwNoPressInkSignalCompareMode != 0` 时参与 enter/exit 判定 |
| `g_asaPrpt.wTx1SignalY` | 函数末尾写入 `wTx1SignalX` | 与 `wTx1SignalX` 相同的镜像值 | `dwNoPressInkSignalCompareMode != 0` 时参与 enter/exit 判定 |
| `g_asaPrpt.wTx1Signal` | 函数末尾写入 `wTx1SignalX` | grid 模式下的 TX1 总信号接口 | `dwNoPressInkSignalCompareMode == 0` 时参与 enter/exit 判定；学习阶段也会缓存 |
| `g_asaPrpt.tx2PeakTable.wSelectedPeak3x3Sum` | `GetGridTx2Peaks()` | TX2 strongest peak 的 3x3 区域总能量 | 先临时写入 `wTx2Signal` 供 `TiltProcess()` 使用 |
| `g_asaPrpt.wTx2SignalX` | 函数末尾写入 `tx2PeakTable.wSelectedPeak3x3Sum` | grid 模式下唯一保留“真 TX2 强度”的导出字段 | 主要出现在日志和 tilt 相关上下文中 |
| `g_asaPrpt.wTx2SignalY` | 函数末尾写入 `wTx1SignalX` | 兼容性回填值，不是独立 TX2 Y 分量 | NoPressInk 核心逻辑不直接依赖它 |
| `g_asaPrpt.wTx2Signal` | 函数末尾写入 `wTx1SignalX` | 最终导出给 NoPressInk 的兼容值 | `GetNoPressInkTiltCompensation()`、prepare learning、`CalcNoPressInkThd()` 会读取它 |
| `g_curASOut.pInnerX[0]` | `TX1CoordinateProcess()` | 当前笔点 refined inner X | `UpdateNoPressInkThold()` 用它查 learned table；历史缓存也会记录 |
| `g_curASOut.pInnerY[0]` | `TX1CoordinateProcess()` | 当前笔点 refined inner Y | 同上，用于 learned table 和历史缓存 |

## 5. NoPressInk 间接依赖的字段表

| 字段 | 来源 | 实际含义 | 在 NoPressInk 中的作用 |
| --- | --- | --- | --- |
| `g_asaPrpt.bDim1SelectedPeakIndex` | `TX1LinePeaksProcess() -> HPP3_NoiseProcess() -> GetRealPeak()` | Dim1/TX 维选中的主线峰 index | `CheckSignalAbnormalStatus()` 以它为中心做 ±2 范围噪声积分 |
| `g_asaPrpt.bDim2SelectedPeakIndex` | 同上 | Dim2/RX 维选中的主线峰 index | 同上，用于另一维噪声窗口 |
| `g_asaPrpt.tx1PeakTable.bPeakCount` | `GetGridTx1Peaks()` | TX1 grid 是否找到有效 peak | 影响 `GridTx1Valid()`，从而影响 `TiltKeepLastFrame()` |
| `g_asaPrpt.tx2PeakTable.bPeakCount` | `GetGridTx2Peaks()` | TX2 grid 是否找到有效 peak | 影响 `GridTx2Valid()` 与 `TiltProcess()` 能否正常启动 |
| `flagTX2Start` | `TiltProcess()` 成功路径置 1 | TX2 tilt 路径是否已经启动 | coordinate revise 功能开启但它未启动时，`NoPressInkHandle()` 会清 `bHadPressurePrev` |
| `g_asaStatic.dwNoPressInkAbnormalFlags` | `CheckSignalAbnormalStatus()` | NPI 专用异常位图 | 阻断学习，也可能导致 no-press ink 状态退出 |

## 6. 与 tied-line 路径的对照

在 `ASA_HPP3TX1TiedGridDataProcesss()` 中，信号字段不是直接用 grid 3x3 sum 回填，而是走：

- `TX1LinePeaksProcess()`
- `TX2LinePeaksProcess()`
- `UpdateLineSignal()`

而 `UpdateLineSignal()` 明确做的是：

```c
wTx1SignalX = line-signal-x;
wTx1SignalY = line-signal-y;
wTx1Signal  = compareMode == 0 ? avg(x, y) : y;

wTx2SignalX = line-signal-x;
wTx2SignalY = line-signal-y;
wTx2Signal  = compareMode == 0 ? avg(x, y) : y;
```

所以：

- 在 tied-line 路径里，`X/Y` 的名字和实际含义是一致的；
- 在 pure grid 路径里，这些字段更像是“给下游兼容使用的统一接口”，不再保留真实分轴语义。

## 7. 当前可以稳定使用的理解

后续如果只看 NoPressInk，在 **grid 模式** 下可以先按下面的方式理解：

| 字段 | 推荐理解 |
| --- | --- |
| `wTx1SignalX` | TX1 主峰 3x3 强度 |
| `wTx1SignalY` | 与 `wTx1SignalX` 相同的镜像值 |
| `wTx1Signal` | TX1 主峰 3x3 强度的总信号接口 |
| `wTx2SignalX` | TX2 主峰 3x3 强度 |
| `wTx2Signal` | 最终导出给 NoPressInk 的兼容值，不应按“纯 TX2 总信号”直接理解 |

## 8. 结论

`NoPressInkProcess` 是 HPP3 压力链路中的状态延续与自学习模块，它依赖 `ASA_HPP3TX1GridDataProcesss` 整理好的信号字段来判断“虽然真实压力掉了，但当前信号是否仍然足够像在书写”。

在当前二进制里，grid 模式下 `wTx1Signal* / wTx2Signal*` 的字段名和最终语义并不完全一致，尤其是 `wTx2Signal` 在函数退出前被重新回填。这一点在重建源码或对照实现时必须优先保持一致，否则 NoPressInk 的 enter/exit、tilt compensation 和学习逻辑都会偏掉。
