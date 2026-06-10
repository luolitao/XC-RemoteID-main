#pragma once
#include <cstdint>
#include "encoder.h"
#include <esp_wifi_types.h>

enum class RID_EncodeMode : uint8_t {
    MODE_GB46750_ONLY = 0,
    MODE_RID_ONLY     = 1,
    MODE_DUAL         = 2
};

struct BroadcastStats {
    uint8_t counter;
    uint32_t total_success;
    uint32_t total_fail;
    uint32_t last_broadcast_ms;
    uint32_t uptime_ms;
};

class WiFi_TX {
public:
    bool init();
    bool transmit(const RIDData &data);
    
    static void printVendorIE(const vendor_ie_data_t* ie);
    
    static void setEncodeMode(RID_EncodeMode mode);
    static RID_EncodeMode getEncodeMode();
    static BroadcastStats getStats(); // 供 WebServer 读取统计数据

private:
    bool _initialised = false;
    uint8_t _mac[6];
};