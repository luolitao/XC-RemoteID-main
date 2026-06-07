/*
 * Wi-Fi 广播发送实现 (Pure ESP-IDF Native Architecture)
 *
 * 使用 esp_wifi_set_vendor_ie 将国标 GB 46750-2025 数据包嵌入 Beacon 帧。
 * 正常飞行模式：AP 热点隐藏，不允许连接，仅用于 RID 广播（§6.1.2）
 */

#include "wifi_tx.h"
#include "../parameters.h"
#include "../encoder.h"

// ==========================================
// ESP-IDF 原生头文件 (替代 Arduino.h 和 WiFi.h)
// ==========================================
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include <string.h>
#include "esp_random.h"

static const char* TAG = "WIFI_TX";

// Vendor IE OUI（自定义，用于国标 RID 标识）
#define RID_OUI_0  0xFA
#define RID_OUI_1  0x0B
#define RID_OUI_2  0xBC
#define RID_OUI_TYPE 0x0D

static uint8_t Counter = 0;
static bool s_wifi_driver_inited = false; // 确保 esp_wifi_init 只调用一次

bool WiFi_TX::init()
{
    if (_initialised) return true;
    _initialised = true;

    // 1. 生成随机本地 MAC，避免追踪
    for (int i = 0; i < 6; i++) _mac[i] = (uint8_t)(esp_random() & 0xFF);
    _mac[0] |= 0x02;   // 本地管理位 (Locally Administered)
    _mac[0] &= 0xFE;   // 单播位 (Unicast)

    // 2. 初始化 Wi-Fi 驱动 (全局只需一次)
    if (!s_wifi_driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init failed or already inited: %s", esp_err_to_name(err));
        }
        s_wifi_driver_inited = true;
    }

    // 3. 设置 AP 接口的 MAC 地址 (不影响 main.cpp 中设置的全局 Base MAC)
    esp_wifi_set_mac(WIFI_IF_AP, _mac);

    // 4. 配置隐藏的 SoftAP (不允许连接)
    char hidden_ssid[32];
    snprintf(hidden_ssid, sizeof(hidden_ssid), "RID-%02X%02X%02X",
             _mac[3], _mac[4], _mac[5]);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, hidden_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(hidden_ssid);
    wifi_config.ap.channel = Parameters::get_uint8(PARAM_WIFI_CH);
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.max_connection = 0; // 0 表示不允许任何设备连接
    wifi_config.ap.ssid_hidden = 1;    // 隐藏 SSID

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    // 5. 设置带宽和发射功率
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    // 功率：满足 §6.1.4 轻型及以上 2.4GHz ≥ 11 dBm (单位 0.25 dBm)
    esp_wifi_set_max_tx_power(52);  // 52 * 0.25 = 13 dBm

    ESP_LOGI(TAG, "Wi-Fi AP initialized (Hidden SSID: %s, CH: %d)", 
             hidden_ssid, wifi_config.ap.channel);
    return true;
}

// 辅助函数：使用 ESP_LOG 打印 Vendor IE (替代 Serial.printf)
void printVendorIE(const vendor_ie_data_t* ie) {
    if (!ie) {
        ESP_LOGW(TAG, "ie: NULL");
        return;
    }

    ESP_LOGI(TAG, "IE: ID=0x%02X, Len=%d, OUI: %02X:%02X:%02X, Type=0x%02X",
             ie->element_id, ie->length,
             ie->vendor_oui[0], ie->vendor_oui[1], ie->vendor_oui[2],
             ie->vendor_oui_type);

    // 使用 ESP-IDF 提供的宏打印十六进制缓冲区，比 for 循环更高效
    uint8_t payload_len = (ie->length >= 4) ? (ie->length - 4) : 0;
    ESP_LOG_BUFFER_HEX(TAG, ie->payload, payload_len > 16 ? 16 : payload_len);
}

bool WiFi_TX::transmit(const RIDData &data)
{
    init();

    Counter = (Counter + 1) & 0xFF;  // 递增计数器
    
    // 编码国标数据包
    uint8_t gb_buf[GB_MAX_PACKET_LEN];
    int gb_len = GB46750Encoder::encode(data, gb_buf, sizeof(gb_buf));
    if (gb_len <= 0) {
        ESP_LOGE(TAG, "Encoder failed");
        return false;
    }

    // 计算 payload 总长度 = Counter(1) + gb_len
    const size_t payload_len = 1 + gb_len;
    const size_t ie_size = sizeof(vendor_ie_data_t) + payload_len;
    
    vendor_ie_data_t *ie = static_cast<vendor_ie_data_t *>(malloc(ie_size));
    if (!ie) {
        ESP_LOGE(TAG, "Malloc failed");
        return false;
    }

    // 填充固定头部
    ie->element_id      = WIFI_VENDOR_IE_ELEMENT_ID;  // 0xDD
    ie->vendor_oui[0]   = RID_OUI_0;
    ie->vendor_oui[1]   = RID_OUI_1;
    ie->vendor_oui[2]   = RID_OUI_2;
    ie->vendor_oui_type = RID_OUI_TYPE;
    ie->length          = (uint8_t)(4 + payload_len); 

    // 先写 Counter，再写国标数据
    ie->payload[0] = Counter;  
    memcpy(ie->payload + 1, gb_buf, gb_len);  

    printVendorIE(ie);

    // 设置 Vendor IE（Beacon + Probe Response）
    // 注意：先清除(false)，再设置(true)
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    bool ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    
    if (ok) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
        ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    }

    free(ie);  // esp_wifi_set_vendor_ie 内部已深拷贝，可安全释放

    if (!ok) {
        ESP_LOGE(TAG, "set_vendor_ie failed");
    }
    
    return ok;
}