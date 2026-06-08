#pragma once

// ==========================================
// 版本控制宏
// ==========================================
#define APP_VER_MAJOR 1
#define APP_VER_MINOR 0
#define APP_VER_PATCH 0

// 是否为开发版 (1 = 开发版带时间戳, 0 = 正式版)
#define APP_IS_DEV_BUILD 1

// 获取格式化后的完整版本号
// 开发版示例: "v1.0.0-dev.20260608.1430"
// 正式版示例: "v1.0.0"
const char* get_full_version();