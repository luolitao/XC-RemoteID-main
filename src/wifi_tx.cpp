#include "wifi_tx.h"
#include "web_server.h"
#include "parameters.h"
#include "encoder.h"
#include "rid_encoder.h"


#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <string.h>
#include <lwip/ip4_addr.h>

static const char* TAG = "WIFI_TX";
#define RID_OUI_0  0xFA
#define RID_OUI_1  0x0B
#define RID_OUI_2  0xBC
#define RID_OUI_TYPE 0x0D

// 内部状态变量
static RID_EncodeMode s_encode_mode = RID_EncodeMode::MODE_GB46750_ONLY;
static uint32_t s_start_time_ms = 0;
static uint8_t s_counter = 0;
static uint32_t s_total_broadcasts = 0;
static uint32_t s_failed_broadcasts = 0;
static uint32_t s_last_broadcast_ms = 0;

// ==========================================
// 接口实现
// ==========================================
void WiFi_TX::setEncodeMode(RID_EncodeMode mode) { s_encode_mode = mode; }
RID_EncodeMode WiFi_TX::getEncodeMode() { return s_encode_mode; }

BroadcastStats WiFi_TX::getStats() {
    return {
        s_counter,
        s_total_broadcasts - s_failed_broadcasts,
        s_failed_broadcasts,
        s_last_broadcast_ms,
        static_cast<uint32_t>(esp_timer_get_time() / 1000 - s_start_time_ms)
    };
}

void WiFi_TX::printVendorIE(const vendor_ie_data_t* ie) {
    if (!ie) return;
    ESP_LOGI(TAG, "IE: ID=0x%02X, Len=%d, OUI: %02X:%02X:%02X, Type=0x%02X",
             ie->element_id, ie->length, ie->vendor_oui[0], ie->vendor_oui[1], ie->vendor_oui[2], ie->vendor_oui_type);
}

// ==========================================
// Wi-Fi 初始化
// ==========================================
bool WiFi_TX::init() {
    if (_initialised) return true;
    _initialised = true;
    s_start_time_ms = esp_timer_get_time() / 1000; 
    ESP_LOGI(TAG, "Initializing Wi-Fi AP...");
    
    // 获取固定的 SoftAP MAC 地址
    esp_read_mac(_mac, ESP_MAC_WIFI_SOFTAP);

    // 1. 初始化网络栈 (这两个函数支持多次调用，安全)
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // 2. ✅ 【核心修复】防止 duplicate key 断言失败
    // 先尝试获取已存在的 AP 接口，如果不存在再创建
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    // ... (后续的 Wi-Fi 驱动初始化、SSID 配置、IP 配置代码保持不变) ...

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mac(WIFI_IF_AP, _mac);

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "XC-RID-%02X%02X%02X", _mac[3], _mac[4], _mac[5]);
    
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid)-1);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = Parameters::get_uint8(PARAM_WIFI_CH);
    wifi_config.ap.max_connection = 4;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    esp_netif_dhcps_start(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"));

    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi AP Started. SSID: %s", ap_ssid);
    
    s_encode_mode = static_cast<RID_EncodeMode>(Parameters::get_uint8(PARAM_ENCODE_MODE));
    
    WebServer::start(); // 启动 Web 服务
    return true;
}

// ==========================================
// 广播核心逻辑 (GB46750 / ASTM)
// ==========================================
bool WiFi_TX::transmit(const RIDData &data) {
    init();
    s_counter = (s_counter + 1) & 0xFF;
    uint8_t payload_buf[256];
    int payload_len = 0;
    bool need_prefix = false; 

    if (s_encode_mode == RID_EncodeMode::MODE_GB46750_ONLY) {
        payload_len = GB46750Encoder::encode(data, payload_buf, sizeof(payload_buf));
        need_prefix = true; 
    } else if (s_encode_mode == RID_EncodeMode::MODE_RID_ONLY) {
        // ASTM 桥接映射
        RIDBasicID basic_id = {}; basic_id.ua_type = RID_UA_HELICOPTER; basic_id.id_type = RID_ID_SERIAL_NO;
        strncpy(basic_id.uas_id, data.uas_id, sizeof(basic_id.uas_id) - 1);
        RIDLocation loc = {}; loc.status = RID_STATUS_AIRBORNE; loc.latitude = data.lat; loc.longitude = data.lon;
        loc.direction = data.track_deg; loc.speed_h = data.ground_speed_ms; loc.speed_v = data.vert_speed_ms;
        loc.altitude_baro = data.baro_alt_m; loc.altitude_geo = data.geo_alt_m; loc.height = data.rel_alt_m;
        loc.height_is_above_ground = true; loc.timestamp_sec = (data.timestamp_ms % 3600000) / 1000.0f;
        
        RIDSystem sys = {}; sys.op_loc_type = RIDSystem::TAKEOFF; sys.op_latitude = data.gcs_lat; sys.op_longitude = data.gcs_lon;
        sys.op_altitude_geo = data.gcs_alt; sys.category_eu = static_cast<RIDSystem::CategoryEU>(data.op_category);
        sys.class_eu = static_cast<RIDSystem::ClassEU>(data.ua_class); sys.timestamp_full = data.timestamp_ms / 1000;
        
        RIDOperatorID op = {}; op.id_type = RIDOperatorID::CAA_ID; 
        strncpy(op.operator_id, data.reg_mark, sizeof(op.operator_id) - 1);
        
        payload_len = RID_Encoder::encodePayload(payload_buf, s_counter, &basic_id, loc, &sys, &op);
    }

    if (payload_len <= 0) return false;

    size_t ie_len = sizeof(vendor_ie_data_t) + (need_prefix ? 1 : 0) + payload_len;
    vendor_ie_data_t *ie = (vendor_ie_data_t *)malloc(ie_len);
    if (!ie) return false;

    ie->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->vendor_oui[0] = RID_OUI_0; ie->vendor_oui[1] = RID_OUI_1; ie->vendor_oui[2] = RID_OUI_2;
    ie->vendor_oui_type = RID_OUI_TYPE;
    ie->length = ie_len - sizeof(vendor_ie_data_t);

    if (need_prefix) {
        ie->payload[0] = s_counter;
        memcpy(ie->payload + 1, payload_buf, payload_len);
    } else {
        memcpy(ie->payload, payload_buf, payload_len);
    }

    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    bool ok = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) == ESP_OK;
    
    s_total_broadcasts++;
    if (ok) s_last_broadcast_ms = esp_timer_get_time() / 1000;
    else s_failed_broadcasts++;

    free(ie);
    return ok;
}