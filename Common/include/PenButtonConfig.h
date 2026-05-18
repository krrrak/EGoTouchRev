#pragma once

#include <cstdint>

/// 笔按键语义模式 — 决定双击触控手势映射到什么行为
enum class PenButtonMode : uint8_t {
    OemCustom    = 0,  ///< OEM 自定义按键码 (VHF HID keycode 0x14B)
    NativeBarrel = 1,  ///< 原生笔杆按键 (Win32 PEN_FLAG_BARREL)
    NativeEraser = 2,  ///< 原生橡皮擦   (Win32 PEN_FLAG_ERASER)
};

/// 笔按键注入路由 — 决定哪些后端参与按键注入
enum class PenButtonRoute : uint8_t {
    VhfOnly     = 0,  ///< 仅 VHF 注入
    Win32Only   = 1,  ///< 仅 Win32 虚拟笔 API 注入
    VhfAndWin32 = 2,  ///< VHF + Win32 双路由（诊断用）
};

inline const char* ToString(PenButtonMode m) {
    switch (m) {
    case PenButtonMode::OemCustom:    return "OEM Custom";
    case PenButtonMode::NativeBarrel: return "Native Barrel";
    case PenButtonMode::NativeEraser: return "Native Eraser";
    default:                          return "Unknown";
    }
}

inline const char* ToString(PenButtonRoute r) {
    switch (r) {
    case PenButtonRoute::VhfOnly:     return "VHF Only";
    case PenButtonRoute::Win32Only:   return "Win32 Only";
    case PenButtonRoute::VhfAndWin32: return "VHF + Win32";
    default:                          return "Unknown";
    }
}
