#pragma once
#include <cstdint>

// ── 用户态驱动侧手写笔状态 ────────────────────────────────────────────────────
// 由 AfeController 维护，仅保留连接生命周期所需状态。
struct StylusState {
    bool    connected = false; // 手写笔已通过 BTMCU 连接
    uint8_t pen_id    = 0;     // 注册的笔 ID（0-3 主表，5=默认）
};
