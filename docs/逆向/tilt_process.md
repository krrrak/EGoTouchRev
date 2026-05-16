# TiltProcess 反编译分析

> **说明:** 以下内容基于 `TSACore.dll` 中的 Ghidra 反编译结果，函数地址 `0x6bac41e6`。`g_asaPrpt->pReserved275_to79F` 内仍有一部分字段未完全命名，下面按访问偏移和上下游写入关系说明其含义。

## 总结

`TiltProcess` 的作用是用 TX1 与 TX2 的坐标差估算笔的 X/Y 倾角。它只在 HPP3 协议下工作；如果 TX1/TX2 数据无效，就直接沿用上一帧 tilt。数据有效时，它会计算 TX2 坐标相对 TX1 坐标的差值，对差值做信号强度相关的长度限制、历史平滑、向量限幅，再通过 `asin(diff / length)` 转成角度，最后做 5 帧平均和 1 度 jitter filter，写入 `g_curASOut->nPreFltXTilt/YTilt` 与 `g_curASOut->nRptXTilt/YTilt`。

在当前关注的 HPP3 grid 路径里，TX1 坐标来自 `TX1CoordinateProcess()`，TX2 坐标来自 `GetGridTx2Peaks()` 的 `dwRefinedX/dwRefinedY`。因此 `TiltProcess` 本质上是在处理：

```text
TX2 refined grid coordinate - TX1 coordinate => coordinate difference => tilt angle
```

## 使用到的结构体与变量

### 1) `g_flagHPP3Protocol`

`TiltProcess` 首先检查：

```c
if (*g_flagHPP3Protocol == 0) return;
```

所以它是 HPP3 专用逻辑，HPP2 不走这里。

### 2) `g_flagDataType`

用于决定有效性检查方式：

| 值 | 数据类型 | 有效性检查 |
| --- | --- | --- |
| `0` | normal line | `LineTx1Valid()` && `LineTx2Valid()` |
| `1` | IQ line | `LineTx1Valid()` && `LineTx2Valid()` |
| `2` | normal grid | `GridTx1Valid()` && `GridTx2Valid()` |
| `3` | tied grid | `LineTx1Valid()` && `LineTx2Valid()` |

如果检查失败，调用 `TiltKeepLastFrame()`，直接复用上一帧：

- `nPreFltXTilt`
- `nPreFltYTilt`
- `nRptXTilt`
- `nRptYTilt`

### 3) `g_prevASOut` / `g_curASOut`

`ASOutRuntime` 中和 tilt 相关的字段：

| 字段 | 含义 |
| --- | --- |
| `dwRptStatusFlags` | 上一帧 report 状态，用于判断是否需要初始化 tilt buffer |
| `wPressure` | 上一帧压力；为 0 时认为刚进入或无有效历史 |
| `nPreFltXTilt` / `nPreFltYTilt` | 当前帧限幅后的原始 tilt |
| `nRptXTilt` / `nRptYTilt` | 当前帧最终输出 tilt |

如果上一帧没有有效状态或压力为 0，`TiltProcess` 会调用 `TiltInit()` 清空 tilt 相关历史 buffer。

### 4) `g_asaPrpt->pReserved275_to79F` 与 `g_coors`

`CoordinateInit()` 中有：

```c
g_coors = g_asaPrpt->pReserved275_to79F + 0x4fb;
```

所以 `TiltProcess` 读到的这些 reserved 偏移实际就是 `g_coors` 数组的一部分：

| 偏移 | 等价字段 | 含义 |
| --- | --- | --- |
| `pReserved + 0x4fb` | `g_coors[0]` | TX1 X 坐标，Q10 单位 |
| `pReserved + 0x4ff` | `g_coors[1]` | TX1 Y 坐标，Q10 单位 |
| `pReserved + 0x513` | `g_coors[6]` | TX2 X 坐标，Q10 单位 |
| `pReserved + 0x517` | `g_coors[7]` | TX2 Y 坐标，Q10 单位 |

在 grid 路径中：

- `TX1CoordinateProcess()` 写 `g_coors[0/1]`
- `GetGridTx2Peaks()` 写 `g_coors[6/7]`
- `TiltProcess()` 计算 `g_coors[6/7] - g_coors[0/1]`

### 5) `g_asaPrpt->wTx1Signal / wTx2Signal`

用于计算 TX1/TX2 信号比例：

```c
ratio = wTx2Signal * 100 / wTx1Signal;
```

若 TX2 信号超过 TX1 的 5 倍，比例被封顶为 `500`。

`TiltProcess` 会把该比例写入 `g_signalRatioBuf`，并取最近 3 帧平均值 `g_signalRatio`。

### 6) tilt 历史 buffer

`TiltInit()` 会清空以下 buffer：

| 变量 | 含义 |
| --- | --- |
| `g_signalRatioBuf[10]` | TX2/TX1 signal ratio 历史 |
| `g_signalRatioBufCnt` | signal ratio 历史帧数，上限 10 |
| `g_coordifdim1Buf[10]` | X 方向 TX2-TX1 坐标差历史 |
| `g_coordifdim2Buf[10]` | Y 方向 TX2-TX1 坐标差历史 |
| `g_coorDifBufCnt` | 坐标差历史帧数，上限 10 |
| `g_tiltdim1Buf[10]` | X tilt 历史 |
| `g_tiltdim2Buf[10]` | Y tilt 历史 |

坐标差和 tilt 输出通常取最近 5 帧平均。

### 7) 参数表相关字段

`GetTX1TX2LenLimit()` 和 `GetTiltByCoorDif()` 会使用：

- `GetTX1TX2LenLimit()`：`g_asaPrmtFlash->bGlobal_tx_pitch`、`g_asaPrmtFlash->field_0xa2a / field_0xa2c`、`g_asaPrmtStylus->pReserved250_to26D`、`g_tsaPrmtFlash[0x1a]`
- `GetTiltByCoorDif()`：按轴使用 `g_asaPrmtFlash->bGlobal_tx_pitch / bGlobal_rx_pitch`，并结合 `g_asaPrmtFlash->field_0xa2a / field_0xa2c`、`g_asaPrmtStylus->pReserved250_to26D`、`g_tsaPrmtFlash[0x1a]`

这些字段共同决定：

1. TX1/TX2 可接受的最大坐标差 `lenLimit`
2. 坐标差换算成角度时使用的轴向长度
3. signal ratio 对 `lenLimit` 的缩放曲线

`g_tsaPrmtFlash[0x1a]` 看起来像轴映射/方向选择标志，会交换 TX/RX 相关分母的使用方式。

## 算法逻辑

### 1) 初始化或保留上一帧

函数只处理 HPP3：

```c
if (!g_flagHPP3Protocol) return;
```

如果上一帧没有有效 report 状态，或上一帧压力为 0，则调用 `TiltInit()` 清空历史：

```c
if (((prev.dwRptStatusFlags & 2) == 0 && (prev.dwRptStatusFlags & 4) == 0) || prev.wPressure == 0) {
    TiltInit();
}
```

随后按 `g_flagDataType` 检查 TX1/TX2 是否都有有效 peak。无效时调用：

```c
TiltKeepLastFrame();
```

这表示本帧不重新计算 tilt，而是直接沿用上一帧输出。

### 2) 计算 TX1/TX2 signal ratio

数据有效后，函数设置：

```c
g_flagTX2Start = 1;
```

然后计算 TX2/TX1 信号比例：

```c
ratio = min(wTx2Signal * 100 / wTx1Signal, 500);
BufTX1TX2SignalRatio(ratio);
g_signalRatio = GetTX1TX2RatioAverage(3);
```

这个 3 帧平均的 `g_signalRatio` 后续用于计算 `lenLimit`。

### 3) 计算坐标差长度限制 `lenLimit`

`GetTX1TX2LenLimit()` 先根据 pitch、stylus 参数和 axis 映射算一个基础长度：

```text
baseLen ~= global_pitch * stylus_param * 0x400 / axis_denominator
```

然后根据 `g_signalRatio` 在 stylus 参数表中做分段缩放：

- 如果 signal ratio 低于首个阈值，`lenLimit = 0`
- 如果处于阈值区间内，线性插值缩放 `baseLen`
- 如果高于最后阈值，保留基础长度

这说明 TX2 信号越弱，允许的 TX1/TX2 坐标差越小；TX2 信号足够强时才允许更大的 tilt 差值。

### 4) 计算 TX2 - TX1 原始坐标差

主差值：

```c
dx = (short)g_coors[6] - (short)g_coors[0];
dy = (short)g_coors[7] - (short)g_coors[1];
```

在 grid 路径里，这就是：

```text
dx = TX2 refined X - TX1 X
dy = TX2 refined Y - TX1 Y
```

函数还会从 TX2 line peak table 中计算一组 alternative difference，用于判断 TX2 当前 strongest peak 是否可能跳点：

```text
altDx = TX2 dim1 strongest peak coordinate - TX1 X
altDy = TX2 dim2 strongest peak coordinate - TX1 Y
```

如果 TX2 strongest slot 和另一个内部选中 slot 不一致，并且 `altDx/altDy` 超过 `lenLimit`，同时已经有历史坐标差，则进入抗跳变逻辑。

### 5) 抗跳变与一阶平滑

如果存在历史坐标差并检测到可疑跳变：

- 若当前 `dx/dy` 也超出 `lenLimit`，直接使用上一帧平滑后的坐标差：

```c
dx = g_coordifdim1Buf;
dy = g_coordifdim2Buf;
```

- 若当前 `dx/dy` 仍在限制内，则做一个 7:1 的低通平滑：

```c
dx = (dx + oldDx * 7) / 8;
dy = (dy + oldDy * 7) / 8;
```

这里的目的应该是避免 TX2 peak 在多个候选峰之间跳动时导致 tilt 突变。

### 6) 单轴限幅或回退历史

随后再次检查 `dx/dy` 是否超过 `lenLimit`：

- 如果没有历史帧，则把每个轴单独 clamp 到 `[-lenLimit, +lenLimit]`
- 如果已有历史帧，则直接回退到上一帧坐标差

这一步是第一层保护，防止单轴差值明显失真。

### 7) 坐标差历史平均

处理后的 `dx/dy` 被写入 10 帧环形/移位 buffer：

```c
BufTX1TX2CoorDif(dx, dy);
```

随后取最近 5 帧平均：

```c
dx = GetTX1TX2CoorDifAverage(5, 0);
dy = GetTX1TX2CoorDifAverage(5, 1);
```

平均值也会回写到：

```c
g_coordifdim1Buf = dx;
g_coordifdim2Buf = dy;
```

所以 buffer 的第 0 项既表示最新输入，也会在后面被最终平滑值覆盖，供下一帧作为“上一帧差值”。

### 8) 坐标差转换成角度

`GetTiltByCoorDif(diff, axis)` 用 `asin()` 转角度：

```c
tilt = asin(diff / axisLen) * 180 / PI;
```

如果 `diff` 超出轴向长度，则饱和到：

- `+90`
- `-90`

这里得到的是预滤波 tilt：

```c
g_curASOut->nPreFltXTilt = GetTiltByCoorDif(dx, 0);
g_curASOut->nPreFltYTilt = GetTiltByCoorDif(dy, 1);
```

### 9) 向量长度限幅

函数接着计算坐标差向量长度：

```c
len = sqrt(dx * dx + dy * dy);
```

如果 `len > lenLimit`，则按比例缩放：

```c
dx = lenLimit * dx / len;
dy = lenLimit * dy / len;
```

然后重新计算 `nPreFltXTilt/YTilt`。这一步是第二层保护：即使单轴不超限，二维合成向量也不能超过允许长度。

### 10) tilt 历史平均与 1 度 jitter filter

预滤波 tilt 写入 tilt buffer：

```c
BufDim1Dim2Tilt(nPreFltXTilt, nPreFltYTilt);
```

如果上一帧压力为 0，或者上一帧 X tilt 为 0，则本帧直接输出预滤波值：

```c
nRptXTilt = nPreFltXTilt;
nRptYTilt = nPreFltYTilt;
```

否则取最近 5 帧 tilt 平均：

```c
nRptXTilt = GetTiltAverage(5, 0);
nRptYTilt = GetTiltAverage(5, 1);
```

最后调用 `Tilt1DegreeJitFilter(prev, cur)`：

- 如果当前值比上一帧大，输出 `cur - 1`
- 如果当前值比上一帧小，输出 `cur + 1`
- 如果相等，保持当前值

也就是说，最终输出会被拉近上一帧 1 度；当变化正好是 1 度时，输出会保持上一帧，从而抑制 1 度抖动。

最终写入：

```c
g_curASOut->nRptXTilt
g_curASOut->nRptYTilt
```

并同步更新：

```c
g_tiltdim1Buf = nRptXTilt;
g_tiltdim2Buf = nRptYTilt;
```

## 与当前 HPP3 grid 路径的关系

在 `ASA_HPP3TX1GridDataProcesss()` 中，调用顺序是：

```text
GetGridTx1Peaks()
TX1CoordinateProcess()
GetGridTx2Peaks()
TiltProcess()
```

因此 grid 模式下的 tilt 输入为：

| 来源 | 写入位置 | 含义 |
| --- | --- | --- |
| `TX1CoordinateProcess()` | `g_coors[0/1]` | TX1 坐标 |
| `GetGridTx2Peaks()` | `g_coors[6/7]` | TX2 refined 坐标 |
| `GetGridTx1Peaks()` | `tx1PeakTable.wSelectedPeak3x3Sum` | TX1 signal |
| `GetGridTx2Peaks()` | `tx2PeakTable.wSelectedPeak3x3Sum` | TX2 signal |

`TiltProcess` 不重新找峰，也不重新计算 TX1/TX2 坐标；它只消费前面步骤已经写好的坐标和信号。

## 需要注意的反编译细节

`UpdatePos()` 的空权重 fallback 在二进制里显示 `dwRefinedX` 和 `dwRefinedY` 都使用 X 方向窗口中心计算；这可能是原始实现的小瑕疵。不过正常情况下只要局部窗口存在高于平均值的有效权重，就会走加权质心分支，不会触发该 fallback。

`TiltProcess` 中对 `g_coors` 的差值计算把 32 位坐标截成 `short` 后再相减。这说明算法期望 tilt 差值在 16 位范围内，或者原始 Q10 坐标本身不会超过该范围。

## 结论

`TiltProcess` 可以概括为：

**根据 TX2 相对 TX1 的坐标偏移估算笔倾角，并用 TX2/TX1 信号比例、坐标差历史、向量限幅、tilt 平均和 1 度 jitter filter 控制输出稳定性。**
