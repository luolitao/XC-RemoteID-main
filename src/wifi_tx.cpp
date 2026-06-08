/*
Wi-Fi 广播发送实现 (支持 RemoteID 广播 + OTA 连接)
【修复说明】
1. 添加 #include <cmath> 修复 fmod 未声明
2. RIDData 字段映射使用占位符 + 注释，按需替换为你的实际字段名
3. 保留原有 GB46750 编码逻辑，RID 编码暂注释（避免编译错误）
*/
#include "wifi_tx.h"
#include "parameters.h"
#include "encoder.h"
// #include "rid_encoder.h"  // 如需启用 RID 编码，取消注释并确保 rid_encoder.h 已修复
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <string.h>
#include "esp_random.h"
#include <lwip/ip4_addr.h>
#include <cmath>  // ✅ 新增：修复 fmod 未声明

static const char* TAG = "WIFI_TX";
#define RID_OUI_0  0xFA
#define RID_OUI_1  0x0B
#define RID_OUI_2  0xBC
#define RID_OUI_TYPE 0x0D

static uint8_t Counter = 0;
static bool s_wifi_driver_inited = false;

bool WiFi_TX::init()
{
    if (_initialised) return true;
    _initialised = true;
    ESP_LOGI(TAG, "Initializing Wi-Fi AP for Broadcast & OTA... ");

    for (int i = 0; i < 6; i++) _mac[i] = (uint8_t)(esp_random() & 0xFF);
    _mac[0] |= 0x02; _mac[0] &= 0xFE;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    if (!s_wifi_driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_driver_inited = true;
    }
    esp_wifi_set_mac(WIFI_IF_AP, _mac);

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "XC-RID-%02X%02X%02X", _mac[3], _mac[4], _mac[5]);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid)-1);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char*)wifi_config.ap.password, "12345678", sizeof(wifi_config.ap.password)-1);
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.channel = Parameters::get_uint8(PARAM_WIFI_CH);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));  // ✅ 修复：删除空格
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP Started. SSID: %s, IP: 10.0.0.1", ap_ssid);

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(52);
    return true;
}

void printVendorIE(const vendor_ie_data_t* ie) {
    if (!ie) return;
    ESP_LOGI(TAG, "IE: ID=0x%02X, Len=%d, OUI: %02X:%02X:%02X, Type=0x%02X",
             ie->element_id, ie->length,
             ie->vendor_oui[0], ie->vendor_oui[1], ie->vendor_oui[2],
             ie->vendor_oui_type);
    uint8_t payload_len = (ie->length >= 4) ? (ie->length - 4) : 0;
    ESP_LOG_BUFFER_HEX(TAG, ie->payload, payload_len > 16 ? 16 : payload_len);
}

bool WiFi_TX::transmit(const RIDData &data)
{
    init();
    Counter = (Counter + 1) & 0xFF;

    // ===== 当前仅启用 GB46750 编码 =====
    uint8_t gb_buf[GB_MAX_PACKET_LEN];
    int gb_len = GB46750Encoder::encode(data, gb_buf, sizeof(gb_buf));
    if (gb_len <= 0) {
        ESP_LOGE(TAG, "GB46750Encoder failed");
        return false;
    }

    const size_t payload_len = 1 + gb_len;
    const size_t ie_size = sizeof(vendor_ie_data_t) + payload_len;
    vendor_ie_data_t *ie = static_cast<vendor_ie_data_t*>(malloc(ie_size));
    if (!ie) return false;

    ie->element_id      = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->vendor_oui[0]   = RID_OUI_0;
    ie->vendor_oui[1]   = RID_OUI_1;
    ie->vendor_oui[2]   = RID_OUI_2;
    ie->vendor_oui_type = RID_OUI_TYPE;
    ie->length          = static_cast<uint8_t>(4 + payload_len);

    ie->payload[0] = Counter;
    memcpy(ie->payload + 1, gb_buf, gb_len);

    if (Counter == 0x00) printVendorIE(ie);

    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    bool ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    if (ok) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
        ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    }

    free(ie);
    return ok;

    // ===== RID 编码模板（按需启用）=====
    /*
    // ⚠️ 请将下方 data.xxx 替换为你 encoder.h 中 RIDData 的实际成员名
    auto to_rid_loc = [&]() -> RIDLocation {
        RIDLocation loc = {};
        loc.status = (data.飞行状态字段 == 1) ? RID_STATUS_AIRBORNE : RID_STATUS_GROUND;
        loc.direction = data.航向字段;
        loc.speed_h = data.水平速度字段;
        loc.speed_v = data.垂直速度字段;
        loc.latitude = data.纬度字段;
        loc.longitude = data.经度字段;
        loc.altitude_baro = data.气压高度字段;
        loc.altitude_geo = data.GPS高度字段;
        loc.height = data.离地高度字段;
        loc.height_is_above_ground = true;
        loc.acc_horiz = RID_ACC_3M; loc.acc_vert = RID_ACC_1M; loc.acc_ts = RID_ACC_1M;
        loc.timestamp_sec = fmod(data.时间戳字段 / 1000.0f, 3600.0f);
        return loc;
    };
    auto to_rid_basic = [&]() -> RIDBasicID {
        RIDBasicID b = {}; b.ua_type = RID_UA_HELICOPTER; b.id_type = RID_ID_SERIAL_NO;
        memset(b.uas_id, 0, 20);
        strncpy(b.uas_id, data.序列号字段, 20);
        return b;
    };

    // 使用 RID_Encoder::encodePayload(...) 编码...
    */
}