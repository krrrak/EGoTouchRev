# HPP3 Pressure Look-ahead 提前切悬浮模式

## 背景

手写笔存在三种状态：

| 状态 | 表现 |
|---|---|
| 无信号 | active stylus 信号消失，进入 release / out 状态 |
| 悬浮 | 仍有 active stylus 信号，但无压力 |
| 接触 | 有 active stylus 信号，且有压力 |

在 TSACore 的 ASA/HPP3 路径中，接触到悬浮不是通过 `ReleaseASAReportExitStylus` 完成的。`ReleaseASAReportExitStylus` 更偏向处理 active signal 消失后的 release。

真正的接触到悬浮切换发生在：

```text
stylus.status 仍为 0x12
但 g_asaStatic.dwAsaStatusFlags 从 7 变为 1
```

含义是：

```text
0x12: HPP3 + active stylus mode
7:    in-range + real pressure + pressure output
1:    in-range only，也就是悬浮
```

## 所属流程

该模式归属于 `ASA_HPP3Process` 中的压力处理阶段。

```c
ASA_HPP3Process()
{
    HPP3_PressureProcess();
    NoPressInkProcess();
    HPP3_PostPressureProcess();
    EdgeCoorProcess();
    EdgeCoorPostProcess();
    HPP3_ASAStaticStatusPostProcess();
}
```

核心实现函数：

```text
HPP3_PressureProcess @ 0x6BABFD1D
```

最终状态落地函数：

```text
HPP3_ASAStaticStatusPostProcess @ 0x6BAC8935
```

蓝牙压力输入函数：

```text
ASA_SetBluetoothPressure @ 0x6BAC714D
TSA_SetASABluetoothPressure @ 0x6BA9CC65
```

## 60Hz BT 到 240Hz HPP3 的压力展开

蓝牙压力真实频率约为 60Hz，而 HPP3 主处理按约 240Hz 运行。因此每个 BT pressure 包提供 4 个压力子采样。

`ASA_SetBluetoothPressure` 每次收到 BT pressure 包时写入：

```c
g_btPressBuf[0] = param_1;
g_btPressBuf[1] = param_2;
g_btPressBuf[2] = param_3;
g_btPressBuf[3] = param_4;
HPP3_ClearBtPressCnt();
```

对应全局：

```text
g_btPressBuf @ 0x6BC707F8
```

每次新 BT 包到达后，`g_btPressCnt` 被清零：

```text
g_btPressCnt @ 0x6BB9EBC2
```

之后每个 240Hz HPP3 frame 会调用 `HPP3_PressureProcess`，并递增 `g_btPressCnt`。

正常情况下，当前应使用哪个 BT pressure slot 由 `GetPressInMapOrder` 决定。

```text
GetPressInMapOrder @ 0x6BABFAEA
```

已知 order table：

```text
g_abBtPressMapOrderOncell = [0, 1, 1, 2, 3, 3]
g_abBtPressMapOrderIncell = [0, 1, 2, 3]
```

## 提前切悬浮的核心逻辑

`HPP3_PressureProcess` 在正常按 order table 取当前 slot 之前，先检查整包最后一个 pressure slot：

```c
g_asaStatic.bHasPressure = 0;

if (g_btPressBuf[3] == 0) {
    g_curASOut.wPressure = 0;
    wPressureRaw = 0;
} else {
    wPressureRaw = GetPressInMapOrder();
    g_curASOut.wPressure = HPP3_GetPressureMapping(wPressureRaw);

    if (g_curASOut.wPressure != 0 && g_prevASOut.wPressure != 0) {
        PressureIIR(0x40);
    }
}

g_btPressCnt++;

HPP3_SuppressBtPressBySignal();

if (g_curASOut.wPressure != 0) {
    g_asaStatic.bHasPressure = 1;
}
```

关键点：

```c
if (g_btPressBuf[3] == 0) {
    g_curASOut.wPressure = 0;
}
```

这就是提前切悬浮的核心。

它的含义是：

```text
如果当前 60Hz BT 包的最后一个 240Hz 子采样已经是 0，
那么即使当前按 order table 本该消费的前面 slot 仍然非零，
也直接将当前 HPP3 frame 判为无压力。
```

因此这是一种 packet-level look-ahead gate。

## 为什么能提前

假设某个 BT pressure 包内容是：

```text
g_btPressBuf = [118, 118, 0, 0]
```

如果只按 `g_btPressCnt` 顺序消费，当前帧可能还会用到 slot 0 或 slot 1，从而继续保持接触状态。

但 `HPP3_PressureProcess` 不这样做。它先看：

```text
g_btPressBuf[3] == 0
```

一旦最后 slot 是 0，就立即：

```text
wPressure = 0
bHasPressure = 0
```

后续 `HPP3_ASAStaticStatusPostProcess` 不再置 pressure/ink 状态位，于是：

```text
dwAsaStatusFlags: 7 -> 1
```

表现为：

```text
接触 -> 悬浮
```

## 状态落地

`HPP3_PressureProcess` 只负责清压力和 `bHasPressure`。

真正把它转换成 ASA 状态的是：

```text
HPP3_ASAStaticStatusPostProcess @ 0x6BAC8935
```

核心逻辑：

```c
if (g_asaStatic.bHasPressure != 0) {
    g_asaStatic.dwAsaStatusFlags |= 2;
}

if (g_curASOut.wPressure != 0) {
    g_asaStatic.dwAsaStatusFlags |= 4;
}
```

所以当 `g_btPressBuf[3] == 0` 时：

```text
g_curASOut.wPressure = 0
g_asaStatic.bHasPressure = 0
```

因此不会置：

```text
dwAsaStatusFlags bit1
dwAsaStatusFlags bit2
```

如果 in-range 仍然有效，最终状态就是：

```text
dwAsaStatusFlags = 1
```

也就是悬浮。

## 与 release 的区别

不要把这个模式和 `ReleaseASAReportExitStylus` 混淆。

`ReleaseASAReportExitStylus` 对应的是 active stylus bit 掉线：

```text
stylus.status: 0x12 -> 0x06
```

这是从 active signal 到 release / no-signal 的处理。

本文描述的接触到悬浮是：

```text
stylus.status 仍然是 0x12
dwAsaStatusFlags: 7 -> 1
```

也就是说：

```text
active signal 仍在，只是 pressure 被提前清零。
```

## 已排除的机制

当前录制中，接触到悬浮不是由以下机制触发：

- `NoPressInkProcess`
- `ReleaseASAReportExitStylus`
- `HPP3_SuppressBtPressBySignal`
- `HPP3_PostPressureProcess` 的 edge signal latch
- fake pressure decrease
- grid valid flag 丢失
- active stylus bit `0x10` 掉线

这些机制可能影响其它场景，但不是当前这个 60Hz BT 到 240Hz HPP3 的提前悬浮模式核心。

## 录制证据

在 `tsacore-recording-20260515-141227.sqlite` 中，按以下条件筛选 contact -> hover：

```text
上一帧:
  stylus.status == 0x12
  dwAsaStatusFlags == 7
  wPressure > 0

下一帧:
  stylus.status == 0x12
  dwAsaStatusFlags == 1
  wPressure == 0
```

共找到 37 次转换。

所有转换均满足：

```text
下一帧 g_btPressBuf[3] == 0
```

典型例子：

```text
scan 6949:
  stylus.status = 0x12
  g_btPressBuf = (380, 380, 380, 380)
  wPressure = 1043
  dwAsaStatusFlags = 7
  状态 = 接触

scan 6950:
  stylus.status = 0x12
  g_btPressBuf = (118, 118, 0, 0)
  wPressure = 0
  dwAsaStatusFlags = 1
  状态 = 悬浮
```

注意：

```text
g_btPressBuf[0] 和 g_btPressBuf[1] 仍然非零，
但 g_btPressBuf[3] 已经为 0，
因此 HPP3_PressureProcess 直接切悬浮。
```

## 总结

该模式可以概括为：

```text
BT pressure 包携带 4 个 240Hz 子采样。
HPP3_PressureProcess 不只消费当前 slot，
而是提前查看最后一个 slot g_btPressBuf[3]。
如果最后 slot 为 0，说明这包数据尾部已经进入抬笔/无压阶段，
于是当前 HPP3 frame 立即清 pressure，
再由 HPP3_ASAStaticStatusPostProcess 将状态从接触 7 切为悬浮 1。
```

归属关系：

```text
输入写入: ASA_SetBluetoothPressure
核心提前判定: HPP3_PressureProcess
正常 slot 映射: GetPressInMapOrder
状态位落地: HPP3_ASAStaticStatusPostProcess
```
