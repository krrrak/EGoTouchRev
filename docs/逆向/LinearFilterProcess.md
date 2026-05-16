# LinearFilterProcess 分析

## 模块总结

`LinearFilterProcess @ 0x6bab1f4b` 是 `TSACore.dll` 中 HPP3 stylus 坐标后处理链路的线性笔画稳定模块。

调用位置：

```text
TSA_ASAProcess
  -> ASA_MainProcess @ 0x6bac6b5f
    -> ASA_CoorPostProcess @ 0x6bac69ef
      -> LinearFilterProcess @ 0x6bab1f4b
```

在 `ASA_CoorPostProcess` 中，`LinearFilterProcess` 是坐标后处理的第一步，输出写入 `g_curASOut.pInnerX[1] / pInnerY[1]`，后续 `CoorReviseProcess`、速度计算、IIR 坐标滤波、坐标后处理等模块都会继续消费这个结果。

该模块不是普通 IIR 或均值滤波，而是一个“基于轨迹直线度判断的状态机式坐标约束器”：

1. 每帧先把原始坐标 `pInnerX/Y[0]` 拷贝到 `pInnerX/Y[1]`。
2. 当触控笔有压力且 HPP3 linear filter feature bit 开启时，维护最近轨迹缓冲。
3. 缓冲点足够多且最小二乘拟合残差足够小后，进入直线约束模式。
4. 进入直线约束时，不会立刻把坐标硬投影到直线，而是通过 enter counter 逐渐增大吸附强度。
5. 保持直线约束时，按拟合直线限幅拉动 `pInnerX/Y[1]`，压制直线绘制时的抖动。
6. 当检测到转弯、反向急折或当前点离拟合线过远时，通过 exit counter 逐渐降低吸附强度并退出。

因此它的主要作用是：在用户画直线或近似直线时增强笔迹直线性，同时通过渐入/渐出机制避免突然吸附或突然释放造成坐标跳变。

## 使用的结构体

### ASOutRuntime

`g_curASOut` 类型为 `ASOutRuntime`，其中与本模块直接相关的字段如下：

| 字段 | 偏移 | 含义 |
|---|---:|---|
| `pInnerX[0]` | `+0x04` | 线性滤波输入 X，原始/前级坐标 |
| `pInnerY[0]` | `+0x28` | 线性滤波输入 Y，原始/前级坐标 |
| `pInnerX[1]` | `+0x08` | 线性滤波输出 X |
| `pInnerY[1]` | `+0x2c` | 线性滤波输出 Y |
| `wPressure` | `+0xd8` | 当前压力；为 0 时线性滤波 reset 并跳过 |

`ASA_MainProcess` 的日志字符串中把 `pInnerX/Y[1]` 标为 `linear`：

```text
TEST X,%d, %d, Prsse:%d,linear:%d,%d,...
```

### AsaLineFitParam

`AsaLineFitParam` 是根据 `UpdateStraightLinePrmt`、`GetPoint2LineDisSquare`、`DragPoint2Line` 和日志字符串还原出的直线拟合参数结构，大小 `0x28`。

| 字段 | 偏移 | 类型 | 含义 |
|---|---:|---|---|
| `dSlopeA` | `+0x00` | `double` | 拟合线斜率 A |
| `dInterceptB` | `+0x08` | `double` | 拟合线截距 B |
| `dNorm` | `+0x10` | `double` | `sqrt(A*A + 1)`，点到线距离归一化分母 |
| `bSwapXY` | `+0x18` | `byte` | 坐标轴交换标志；0 表示 `y = A*x + B`，1 表示 `x = A*y + B` |
| `nSumDistSq` | `+0x1c` | `int` | 缓冲点到拟合线距离平方和 |
| `nMaxDistSq` | `+0x20` | `int` | 最大距离平方 |
| `nLastDistSq` | `+0x24` | `int` | 最后一个点距离平方 |

`UpdateStraightLinePrmt` 会根据 X/Y 方差自动选择用 `y = A*x + B` 还是 `x = A*y + B` 表示直线，避免接近垂直线时斜率不稳定。

### AsaStaticPressureRuntime

`g_asaStatic` 中与线性滤波相关的字段如下：

| 字段 | 偏移 | 类型 | 含义 |
|---|---:|---|---|
| `dwHpp3FeatureFlags` | `+0x54` | `uint` | HPP3 feature flags；linear filter 使用 bit `0x20` |
| `dwLinearFilterState` | `+0x60` | `uint` | 线性滤波状态机状态 |
| `nLinearSegStartX` | `+0x64` | `int` | 当前直线段起点 X，用于角度判断 |
| `nLinearSegStartY` | `+0x68` | `int` | 当前直线段起点 Y，用于角度判断 |
| `nLinearSegLastX` | `+0x6c` | `int` | 最近直线缓冲点 X |
| `nLinearSegLastY` | `+0x70` | `int` | 最近直线缓冲点 Y |
| `nLinearAnchorX` | `+0x74` | `int` | enter/exit 过程中用于计数递减的锚点 X |
| `nLinearAnchorY` | `+0x78` | `int` | enter/exit 过程中用于计数递减的锚点 Y |
| `stPrevLineFit` | `+0x80` | `AsaLineFitParam` | 不包含当前点的拟合线，日志名 `LineFit` |
| `stCurLineFit` | `+0xa8` | `AsaLineFitParam` | 包含当前点的拟合线，日志名 `cur` |
| `wLinearEnterCnt` | `+0xd0` | `ushort` | 进入直线约束的渐入计数器，最大 10 |
| `wLinearExitCnt` | `+0xd2` | `ushort` | 退出直线约束的渐出计数器，最大 10 |

### AsaPrmtFlashPressureRuntime

`g_asaPrmtFlash` 中与该模块相关的参数字段如下：

| 字段 | 偏移 | 类型 | 含义 |
|---|---:|---|---|
| `dwHpp3FeatureFlagsA` | `+0xa40` | `uint` | `ASAStaticInit` 初始化 feature flags 的来源之一 |
| `dwHpp3FeatureFlagsB` | `+0xa44` | `uint` | `ASAStaticInit` 初始化 feature flags 的来源之一 |
| `wLinearDragLimit` | `+0xa62` | `ushort` | 每帧最大吸附/拖拽距离基准 |
| `wLinearEnterMaxDistSq` | `+0xa64` | `ushort` | 判断轨迹足够直、可进入直线模式的 max distance² 阈值 |
| `wLinearExitDistSq` | `+0xa66` | `ushort` | 当前点离线过远、需要退出直线模式的 distance² 阈值 |

## 关键变量

### Feature bit

线性滤波是否启用由 `g_asaStatic.dwHpp3FeatureFlags & 0x20` 决定。

相关函数：

| 函数 | 地址 | 行为 |
|---|---:|---|
| `ASA_EnableHpp3LinearFilterFeature` | `0x6bac8be0` | `dwHpp3FeatureFlags |= 0x20` |
| `ASA_DisableHpp3LinearFilterFeature` | `0x6bac8bff` | `dwHpp3FeatureFlags &= ~0x20` |
| `ASA_IsHpp3LinearFilterFeatureEnabled` | `0x6bac8c1e` | 返回 bit `0x20` 是否置位 |

`ASAStaticInit @ 0x6bac8481` 初始化时会把 `g_asaPrmtFlash + 0xa40` 与 `+0xa44` 的 flags OR 到 `g_asaStatic.dwHpp3FeatureFlags`。

### 轨迹缓冲

#### g_asaStraightLine* buffer

| 变量 | 地址 | 含义 |
|---|---:|---|
| `g_asaStraightLineXBuf` | `0x6bb9d480` | 直线检测用 X 缓冲 |
| `g_asaStraightLineYBuf` | `0x6bb9dac0` | 直线检测用 Y 缓冲 |
| `g_asaStraightLineFrameBuf` | `0x6bb9e100` | 对应 frame index 缓冲 |
| `g_asaStraightLineBufCnt` | `0x6bb9e420` | 直线缓冲有效计数 |

由 `BufStraightPaintPoint @ 0x6bab0836` 更新。

更新条件：当前原始点相对最后入缓冲点满足以下任一条件：

```text
abs(dx) > 64 || abs(dy) > 64
```

计数最大限制为 400。在线性滤波 state 为 `4` 或 `6` 时，函数仍会更新最后点，但不会增加 `g_asaStraightLineBufCnt`。

#### g_asaShortDis* buffer

| 变量 | 地址 | 含义 |
|---|---:|---|
| `g_asaShortDisXBuf` | `0x6bb9e440` | 短距离 X 缓冲 |
| `g_asaShortDisYBuf` | `0x6bb9e4c0` | 短距离 Y 缓冲 |
| `g_asaShortDisFrameBuf` | `0x6bb9e520` | 对应 frame index 缓冲 |
| `g_asaShortDisBufCnt` | `0x6bb9e548` | 短距离缓冲计数 |

由 `BufShortDistancePoint @ 0x6bab09ad` 更新。

更新条件：

```text
abs(dx) > 16 || abs(dy) > 16
```

在本组线性滤波函数中，短距离 buffer 没直接参与 state 判断，更像是保留给其它逻辑或诊断使用。

### 状态机状态

`g_asaStatic.dwLinearFilterState` 的状态含义：

| state | 含义 | 处理函数 |
|---:|---|---|
| `0` | 初始预热 | `LinearFilterProcess` 内部直接转 `1` |
| `1` | 预热 | `LinearFilterProcess` 内部直接转 `2` |
| `2` | 预热 | `LinearFilterProcess` 内部直接转 `3` |
| `3` | 曲线/等待直线检测 | `CurveLineProcess` |
| `4` | 渐入直线吸附 | `EnterStraightLineProcess` |
| `5` | 保持直线吸附 | `StraightLineProcess` |
| `6` | 渐出直线吸附 | `ExitStraightLineProcess` |
| `>6` | 异常状态 | 强制回 `3` |

## 详细逻辑

### 1. 入口处理

`LinearFilterProcess` 每帧先执行：

```c
pInnerX[1] = pInnerX[0];
pInnerY[1] = pInnerY[0];
```

也就是说，如果后续没有进入吸附逻辑，线性滤波输出等于输入。

随后检查两个条件：

1. `g_curASOut.wPressure != 0`
2. `ASA_IsHpp3LinearFilterFeatureEnabled() == true`

只要任一条件不满足，就 reset：

```c
g_asaStraightLineBufCnt = 0;
g_asaShortDisBufCnt = 0;
g_asaStatic.dwLinearFilterState = 0;
return;
```

### 2. 更新拟合线

当 `g_asaStraightLineBufCnt > 19` 时，会更新两条拟合线：

```c
UpdateStraightLinePrmt(&g_asaStatic.stCurLineFit,  1, 400 - g_asaStraightLineBufCnt);
UpdateStraightLinePrmt(&g_asaStatic.stPrevLineFit, 0, 400 - g_asaStraightLineBufCnt);
```

区别：

- `stCurLineFit` 包含当前输入点 `pInnerX/Y[0]`。
- `stPrevLineFit` 不包含当前输入点，只基于历史缓冲点。

这样可以同时用于：

- 判断当前点是否仍然符合直线趋势。
- 在稳定直线状态中使用上一帧/历史趋势进行吸附，避免当前异常点直接污染吸附线。

### 3. state 0/1/2：预热

前三个状态不做复杂处理，只是逐帧推进：

```text
0 -> 1
1 -> 2
2 -> 3
```

它们的作用是给轨迹缓冲和前级坐标处理留出初始化时间，避免刚落笔时立即进入直线判断。

### 4. state 3：CurveLineProcess，等待直线成立

`CurveLineProcess @ 0x6bab1879` 负责判断当前轨迹是否足够直。

关键逻辑：

```c
if (g_asaStraightLineBufCnt > 0x14) {
    g_asaStraightLineBufCnt = 0x14;
}

if (g_asaStraightLineBufCnt > 0x13 &&
    g_asaStatic.stCurLineFit.nMaxDistSq < g_asaPrmtFlash->wLinearEnterMaxDistSq) {
    g_asaStatic.nLinearAnchorX = pInnerX[0];
    g_asaStatic.nLinearAnchorY = pInnerY[0];
    g_asaStatic.dwLinearFilterState = 4;
    g_asaStatic.wLinearEnterCnt = 10;
}
```

含义：

- 至少需要 20 个 straight-line buffer 点。
- 当前拟合线的最大距离平方必须小于 `wLinearEnterMaxDistSq`。
- 条件满足后进入 state 4，即渐入直线吸附。
- enter counter 初始化为 10。

这里使用的是 `stCurLineFit`，也就是包含当前点的拟合结果，因此进入条件要求“当前点加入后整段轨迹仍足够直”。

### 5. state 4：EnterStraightLineProcess，渐入直线吸附

`EnterStraightLineProcess @ 0x6bab190d` 负责逐渐增强吸附强度。

#### 5.1 enter counter 递减

如果 `wLinearEnterCnt == 0`，状态切到 `5`：

```c
if (wLinearEnterCnt == 0) {
    state = 5;
}
```

如果 counter 尚未归零，则只有当前点相对 anchor 移动超过 32 subpixel 时才递减：

```text
abs(anchorX - pInnerX[0]) > 32 || abs(anchorY - pInnerY[0]) > 32
```

递减后更新 anchor：

```c
wLinearEnterCnt--;
anchorX = pInnerX[0];
anchorY = pInnerY[0];
```

因此 enter counter 不是单纯按帧递减，而是按有效位移递减。

#### 5.2 当前点离线过远则转入 exit

计算当前点到 `stCurLineFit` 的距离平方：

```c
distSq = GetPoint2LineDisSquare(&stCurLineFit, pInnerX[0], pInnerY[0]);
```

如果：

```text
distSq > wLinearExitDistSq
```

则进入 state 6：

```c
state = 6;
wLinearExitCnt = 10 - wLinearEnterCnt;
```

这样如果刚开始渐入就发现轨迹不再直，会用与已进入程度匹配的 exit counter 平滑退出。

#### 5.3 渐入吸附强度

调用：

```c
DragPoint2Line(&stCurLineFit,
    wLinearDragLimit - (wLinearDragLimit * wLinearEnterCnt) / 10);
```

等价于：

```text
dragLimit = wLinearDragLimit * (10 - wLinearEnterCnt) / 10
```

所以：

- 刚进入 state 4 时，`wLinearEnterCnt = 10`，`dragLimit = 0`。
- 随着 counter 递减，吸附强度逐渐增大。
- counter 到 0 后，吸附强度达到 `wLinearDragLimit`。

### 6. state 5：StraightLineProcess，保持直线吸附

`StraightLineProcess @ 0x6bab1ad1` 是稳定直线状态。

#### 6.1 更新直线段端点

从 straight-line buffer 中取当前段起点：

```c
nLinearSegStartX = g_asaStraightLineXBuf[400 - g_asaStraightLineBufCnt];
nLinearSegStartY = g_asaStraightLineYBuf[400 - g_asaStraightLineBufCnt];
```

最近点来自：

```c
nLinearSegLastX = DAT_6bb9dabc;
nLinearSegLastY = DAT_6bb9e0fc;
```

#### 6.2 当前点距离判断

计算当前点到 `stCurLineFit` 的距离平方：

```c
distSq = GetPoint2LineDisSquare(&stCurLineFit, pInnerX[0], pInnerY[0]);
```

如果大于 `wLinearExitDistSq`，会退出直线状态。

#### 6.3 转角判断

`GetTwoLineAngle` 不是返回角度，而是返回：

```text
cos(theta) * 1000
```

判定条件：

```c
angleCos1000 = GetTwoLineAngle(...);

if (angleCos1000 < 700 || distSq > wLinearExitDistSq) {
    state = 6;
    wLinearExitCnt = 10;
}
```

`700` 约等于 `cos(45.6°) * 1000`，因此当方向变化超过约 45° 时，会退出直线吸附。

如果：

```text
angleCos1000 < -500
```

约等于反向夹角超过 120°，会额外递减 exit counter 并缩小当前帧的 drag limit。

#### 6.4 使用历史拟合线吸附

稳定状态调用：

```c
DragPoint2Line(&stPrevLineFit, dragLimit);
```

也就是说，在保持直线吸附时使用的是不包含当前点的历史拟合线 `stPrevLineFit`，这样可以避免当前异常点直接影响吸附直线。

### 7. state 6：ExitStraightLineProcess，渐出直线吸附

`ExitStraightLineProcess @ 0x6bab1d2a` 负责逐渐降低直线吸附强度。

#### 7.1 exit counter 归零后回到 state 3

```c
if (wLinearExitCnt == 0) {
    state = 3;
    g_asaStraightLineBufCnt = 0x14;
}
```

退出后回到曲线/等待直线检测状态。

#### 7.2 按有效位移递减 exit counter

与 enter counter 类似，只有当前点相对 anchor 移动超过 32 subpixel 时才递减：

```text
abs(anchorX - pInnerX[0]) > 32 || abs(anchorY - pInnerY[0]) > 32
```

递减后，如果方向出现反向急折：

```text
GetTwoLineAngle(...) < -500
```

可能额外再递减一次。

#### 7.3 可重新进入 state 4

即使已经在 exit 状态，只要当前拟合线重新满足直线条件：

```c
stCurLineFit.nMaxDistSq < wLinearEnterMaxDistSq
```

就会重新进入 state 4：

```c
state = 4;
wLinearEnterCnt = 10 - wLinearExitCnt;
```

这使得算法可以在“短暂偏离后又恢复直线”的情况下平滑回到直线吸附，而不是彻底退出后重新预热。

#### 7.4 渐出吸附强度

调用：

```c
DragPoint2Line(&stCurLineFit,
    wLinearExitCnt * wLinearDragLimit / 10);
```

所以：

- exit counter 越大，吸附越强。
- 随着 counter 递减，吸附逐渐减弱。
- counter 到 0 后完全退出直线吸附。

### 8. DragPoint2Line 的数学逻辑

`DragPoint2Line @ 0x6bab15d8` 的输入是：

```c
DragPoint2Line(AsaLineFitParam *pLineFit, ushort wMaxDrag)
```

它读取当前线性滤波输出点：

```c
x = pInnerX[1];
y = pInnerY[1];
```

计算点到线的有符号垂直距离，然后 clamp 到：

```text
[-wMaxDrag, +wMaxDrag]
```

最后沿法线方向移动当前输出点：

```c
pInnerX[1] = x + normalX * clampedDistance;
pInnerY[1] = y + normalY * clampedDistance;
```

因此它不是“一次性投影到直线”，而是“每帧最多移动 `wMaxDrag` 的限幅吸附”。

一个需要特别注意的细节：

- `GetPoint2LineDisSquare` 使用传入 `pLineFit->bSwapXY`。
- 但 `DragPoint2Line` 的汇编读取的是 `g_asaStatic.stPrevLineFit.bSwapXY`，不是传入 `pLineFit->bSwapXY`。

这意味着即使 state 4/6 传入的是 `stCurLineFit`，拖拽方向分支仍受 `stPrevLineFit.bSwapXY` 控制。若两条拟合线的 `bSwapXY` 一致则没有影响；若不一致，则可能出现隐藏耦合或潜在 bug。

### 9. UpdateStraightLinePrmt 的拟合逻辑

`UpdateStraightLinePrmt @ 0x6bab0af0` 对 straight-line buffer 做最小二乘拟合。

输入参数：

```c
void UpdateStraightLinePrmt(
    AsaLineFitParam *pLineFit,
    byte bIncludeCurrentPoint,
    ushort wStartIndex)
```

核心步骤：

1. 累加 buffer 点相对最后缓冲点的 X/Y 偏移。
2. 根据 X/Y 方差选择拟合形式：
   - X 方差更大：使用 `y = A*x + B`，`bSwapXY = 0`
   - Y 方差更大：使用 `x = A*y + B`，`bSwapXY = 1`
3. 计算：
   - `dSlopeA`
   - `dInterceptB`
   - `dNorm = sqrt(A*A + 1)`
4. 遍历参与拟合的点，计算每个点到拟合线的距离平方。
5. 写入：
   - `nSumDistSq`
   - `nMaxDistSq`
   - `nLastDistSq`
6. 如果 `bIncludeCurrentPoint == 1`，还会把当前输入点纳入 residual 统计。

### 10. 输出裁剪

在状态机处理、日志和 buffer 更新后，`LinearFilterProcess` 会对 `pInnerX/Y[1]` 做边界裁剪：

```c
pInnerX[1] = clamp(pInnerX[1], 0, bTxCount * 1024);
pInnerY[1] = clamp(pInnerY[1], 0, bRxCount * 1024);
```

这保证线性滤波输出不会超出传感器坐标范围。

## 总结

`LinearFilterProcess` 的核心是：

```text
直线检测 -> 渐入吸附 -> 稳定吸附 -> 渐出吸附
```

它通过 straight-line buffer 和最小二乘拟合判断轨迹是否足够直；通过 `wLinearDragLimit` 控制每帧最大吸附距离；通过 `wLinearEnterCnt / wLinearExitCnt` 平滑进入和退出；通过 `wLinearEnterMaxDistSq / wLinearExitDistSq` 区分进入直线与退出直线的阈值。

该模块位于坐标后处理链路前端，因此其输出 `pInnerX/Y[1]` 会直接影响后续坐标修正、IIR 滤波、屏幕映射和最终上报坐标。
