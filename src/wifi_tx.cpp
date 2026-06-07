/*
 * Wi-Fi 广播发送实现 (支持 RemoteID 广播 + OTA 连接)
 */
#include "wifi_tx.h"
#include "parameters.h"
#include "encoder.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <string.h>
#include "esp_random.h"
#include <lwip/ip4_addr.h>

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

    ESP_LOGI(TAG, "Initializing Wi-Fi AP for Broadcast & OTA...");

    // 1. 生成随机本地 MAC (避免追踪)
    for (int i = 0; i < 6; i++) _mac[i] = (uint8_t)(esp_random() & 0xFF);
    _mac[0] |= 0x02;   // 本地管理位
    _mac[0] &= 0xFE;   // 单播位

    // 2. 初始化网络接口和事件循环 (OTA 必须)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    // 3. 初始化 Wi-Fi 驱动
    if (!s_wifi_driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_driver_inited = true;
    }

    // 4. 设置 AP 接口的 MAC 地址
    esp_wifi_set_mac(WIFI_IF_AP, _mac);

    // 5. 配置可见的 SoftAP (允许连接，用于 OTA)
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "XC-RID-%02X%02X%02X", _mac[3], _mac[4], _mac[5]);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    // 如果您希望有密码，可以取消下面两行的注释并设置密码
    strncpy((char*)wifi_config.ap.password, "12345678", sizeof(wifi_config.ap.password));
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // wifi_config.ap.authmode = WIFI_AUTH_OPEN; // 无密码，方便测试
    wifi_config.ap.channel = Parameters::get_uint8(PARAM_WIFI_CH);
    wifi_config.ap.max_connection = 4;        // 【关键修改】允许最多 4 个设备连接
    wifi_config.ap.ssid_hidden = 0;           // 【关键修改】0 = 可见，1 = 隐藏

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // 6. 配置静态 IP (10.0.0.1) 以便 OTA 访问
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    // 7. 启动 Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP Started. SSID: %s, IP: 10.0.0.1", ap_ssid);

    // 8. 设置发射功率
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(52);  // 13 dBm

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
    
    uint8_t gb_buf[GB_MAX_PACKET_LEN];
    int gb_len = GB46750Encoder::encode(data, gb_buf, sizeof(gb_buf));
    if (gb_len <= 0) {
        ESP_LOGE(TAG, "Encoder failed");
        return false;
    }

    const size_t payload_len = 1 + gb_len;
    const size_t ie_size = sizeof(vendor_ie_data_t) + payload_len;
    
    vendor_ie_data_t *ie = static_cast<vendor_ie_data_t *>(malloc(ie_size));
    if (!ie) return false;

    ie->element_id      = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->vendor_oui[0]   = RID_OUI_0;
    ie->vendor_oui[1]   = RID_OUI_1;
    ie->vendor_oui[2]   = RID_OUI_2;
    ie->vendor_oui_type = RID_OUI_TYPE;
    ie->length          = (uint8_t)(4 + payload_len);

    ie->payload[0] = Counter;  
    memcpy(ie->payload + 1, gb_buf, gb_len);  

    printVendorIE(ie);

    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    bool ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    
    if (ok) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
        ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    }

    free(ie);
    return ok;
}