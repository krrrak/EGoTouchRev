# TX1CoordinateProcess 反编译分析

> **说明:** 以下内容基于 `TSACore.dll` 中的 Ghidra 反编译结果，函数地址 `0x6baaf06d`。其中 `g_asaGroupInfo` 和部分 peak-table 缓冲区尚未被完整命名，下面的字段含义根据读写方式推断。

## 总结

`TX1CoordinateProcess` 的作用是把 TX1 的一维峰值结果转换成二维坐标。它从 `g_asaGroupInfo[2]` / `g_asaGroupInfo[3]` 读取 TX1 的 X/Y 线型峰表，在三角插值和重力插值之间切换，先算出 Q10 形式的原始坐标，再做多项式补偿和边界裁剪，最后通过 sensor pitch 标定表映射到物理坐标，写入 `g_curASOut->pInnerX[3] / pInnerY[3]`，并复制到 `[0]` 作为后续流程的主输出。

这段函数只被 `ASA_HPP3TX1GridDataProcesss()` 调用，所以它只出现在 HPP3 的 grid 数据路径里。

## 使用到的结构体与变量

### 1) `g_asaGroupInfo`：峰值组描述表（推断）

`g_asaGroupInfo` 在 `RawInit()` 中被初始化为 6 个 `0x40` 字节的 group entry。`TX1CoordinateProcess` 只用到 group 2 和 group 3。

| 偏移 | 含义 | 在本函数中的用途 |
| --- | --- | --- |
| `+0x08` | 搜索 profile 指针 | `GetCoordinateByTriangleOf()` / `GetCoordinateByGravityOf()` 读取坐标搜索序列 |
| `+0x28` | 峰表 / 峰记录缓冲区指针 | 读取 `byte 2` 作为当前峰中心索引 |
| `+0x38` | profile 长度 | 作为 `txCount` / `rxCount` 的边界 |

group 2 对应 TX1 的 X 轴，group 3 对应 TX1 的 Y 轴。

### 2) `g_asaPrmt`：坐标和边缘补偿参数

`g_asaPrmt` 是一组 40 字节的运行参数块，函数里用到：

- `g_asaPrmt[1].wField08`
  - bit 0：坐标求解模式选择
    - `0` → 三角法
    - `1` → 重力法
  - bit 3 (`0x8`)：`EdgeCompensating()` 的附加边缘修正开关
- `g_asaPrmt[5]`：X 轴多项式补偿系数起点，传给 `CoorMultiOrderFitCompensate()`
- `g_asaPrmt[6].qwField18`：Y 轴多项式补偿系数起点，传给 `CoorMultiOrderFitCompensate()`
- `g_asaPrmt[14].qwField18`：边缘补偿系数起点，传给 `GetFictiousEdge()` / `EdgeCompensating()`
- `g_asaPrmt->bTxCount` / `bRxCount`：X/Y 方向的有效传感器数量，用于裁剪边界

### 3) `g_coors`

`g_coors` 是一个二维坐标临时缓冲区：

- `g_coors[0]`：TX1 X 轴原始坐标，Q10 单位
- `g_coors[1]`：TX1 Y 轴原始坐标，Q10 单位

这里的 Q10 表示每个 sensor pitch 被拆成 1024 份，因此一个完整 cell 的范围是 `0x400`。

### 4) `g_curASOut` / `ASOutRuntime`

`g_curASOut` 是当前输出帧：

- `pInnerX[3]` / `pInnerY[3]`：本函数写入的映射后坐标
- `pInnerX[0]` / `pInnerY[0]`：复制自 `[3]`，作为后续流程的主坐标输出

### 5) `g_tsaPrmtFlash`

- `g_tsaPrmtFlash + 0xA0`：Dim1 的 pitch 映射表
- `g_tsaPrmtFlash + 0x320`：Dim2 的 pitch 映射表

`SensorPitchSizeMapDim1/2()` 只是把 Q10 坐标插值成物理 pitch 值。

### 6) 峰表相关辅助结构

`GetCoordinateByTriangleOf()` / `GetCoordinateByGravityOf()` 会读 `group->peakTable[2]` 作为当前峰中心索引。Ghidra 目前把这块内存标成 reserved 区域，但从使用方式看，它保存的是“当前用于坐标计算的峰中心”。

## 算法逻辑

### 1) 选择求解方式

函数先读：

```c
mode = g_asaPrmt[1].wField08 & 1;
```

- `mode == 0`：使用三角法
- `mode == 1`：使用重力法

然后分别对 group 2 / group 3 求一次坐标：

- group 2 → `g_coors[0]`
- group 3 → `g_coors[1]`

### 2) 三角法分支

`GetCoordinateByTriangleOf(groupId)` 的核心是把“峰中心所在 cell”加上一个 `0 ~ 1024` 的子像素偏移。

- 先从 `group->peakTable[2]` 取出当前峰中心索引 `peakIdx`
- 如果峰不在最左 / 最右：
  - 调用 `TriangleAlgUsing3Piont(left, center, right)`
  - 返回值以 `0x200` 为中心，表示 cell 内的细分位置
  - 最终坐标 = `peakIdx * 0x400 + subOffset`
- 如果峰在边缘：
  - 先用 `GetFictiousEdge()` 生成一个“虚拟边缘点”
  - 再用 `TriangleAlgEdge()` 做边缘三角插值
  - 右边缘会用 `profileLen * 0x400 - offset` 的方式镜像回来

`TriangleAlgEdge()` 还有一个弱信号保护：如果三点总和低于阈值，会把结果直接压成 `0`。

### 3) 重力法分支

`GetCoordinateByGravityOf(groupId)` 的逻辑更像“加权质心”：

- 从 `group->peakTable[2]` 取当前峰中心索引
- 用 `GetFictiousEdge()` 生成边缘虚拟值
- `UpdateTX1GravityData()` 构造一个局部权重窗口：
  - 非边缘时一般取峰点左右各 1 个点
  - 边缘时会扩展成 3~4 个点，并用虚拟边缘值补齐
  - 返回值的高字节是窗口起始索引，低字节是窗口长度
- `Gravity()` 对该窗口做加权平均，得到 cell 内的子像素偏移
- 最终坐标 = `startIndex * 0x400 + centroidOffset`

### 4) 多项式补偿

得到 `g_coors[0]` / `g_coors[1]` 后，函数分别调用：

```c
g_coors[0] = CoorMultiOrderFitCompensate(g_coors[0], g_asaPrmt + 5);
g_coors[1] = CoorMultiOrderFitCompensate(g_coors[1], &g_asaPrmt[6].qwField18);
```

它的思路是：

- 先取当前 cell 内的位置 `coord % 0x400`
- 计算它到 cell 中心 `0x200` 的距离
- 用四阶以内的多项式计算补偿量
- 如果点在右半边，补偿量取负

也就是说，它是在 cell 内做一个关于中心对称的非线性修正。

### 5) 边界裁剪

补偿后立即裁剪：

- X：`0 <= g_coors[0] <= g_asaPrmt->bTxCount * 0x400 - 1`
- Y：`0 <= g_coors[1] <= g_asaPrmt->bRxCount * 0x400 - 1`

这一步保证坐标不会跑出有效传感器范围。

### 6) 物理坐标映射

最后把 Q10 坐标映射到实际 pitch：

```c
pInnerX[3] = SensorPitchSizeMapDim1(g_coors[0], 0x400);
pInnerY[3] = SensorPitchSizeMapDim2(g_coors[1], 0x400);
pInnerX[0] = pInnerX[3];
pInnerY[0] = pInnerY[3];
```

`SensorPitchSizeMap()` 本质上是按标定表做线性插值，所以这里得到的是经过传感器 pitch 标定后的物理坐标。

### 7) 结果输出

`TX1CoordinateProcess` 本身不返回状态码，它通过写全局缓冲区完成输出。它的有效结果就是：

- `g_coors[0/1]`：原始 Q10 坐标
- `g_curASOut->pInnerX[0] / pInnerY[0]`：最终坐标
- `g_curASOut->pInnerX[3] / pInnerY[3]`：同一结果的中间落点

## 结论

`TX1CoordinateProcess` 做的事可以概括成一句话：

**从 TX1 的二维峰表里挑出 X/Y 峰中心，按配置选择三角法或重力法求出 Q10 坐标，再做非线性补偿、边界裁剪和 pitch 映射，输出到当前帧。**
