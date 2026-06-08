#pragma once
#include <cstdint>
#include "encoder.h" // 确保包含你的 RIDData 定义

// 编码模式枚举
enum class RID_EncodeMode : uint8_t {
    MODE_GB46750_ONLY = 0,
    MODE_RID_ONLY     = 1,
    MODE_DUAL         = 2
};

class WiFi_TX {
public:
    bool init();
    bool transmit(const RIDData &data);
    void setEncodeMode(RID_EncodeMode mode);
    RID_EncodeMode getEncodeMode() const;

private:
    bool _initialised = false;
    uint8_t _mac[6];
};