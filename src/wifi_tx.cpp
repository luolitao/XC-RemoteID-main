/*
Wi-Fi 广播发送实现 (支持 RemoteID 广播 + 统一 Web 服务)
【修复说明】
2. 补充 esp_timer.h 修复 esp_timer_get_time 未声明
3. 补全 _http_ota_update 类内声明
4. 修复不存在的 esp_wifi_ap_get_record API
*/
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <string.h>
#include "esp_random.h"
#include <lwip/ip4_addr.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"  // ✅ 新增：修复 esp_timer_get_time 未声明

#include "wifi_tx.h"
#include "parameters.h"
#include "encoder.h"
#include "rid_encoder.h"
#include "mock_data.h"

#define RID_OUI_0  0xFA
#define RID_OUI_1  0x0B
#define RID_OUI_2  0xBC
#define RID_OUI_TYPE 0x0D
#define HTTP_SERVER_PORT 80
#define HTTP_MAX_REQ_LEN 4096
#define WEB_AUTH_TOKEN "admin123"

extern RIDData mock_rid_data; // 引用 main.cpp 中的全局变量
static const char* TAG = "WIFI_TX";

static uint8_t Counter = 0;
static bool s_wifi_driver_inited = false;
static RID_EncodeMode s_encode_mode = RID_EncodeMode::MODE_GB46750_ONLY;
static uint32_t s_start_time_ms = 0;

// ✅ 修复：改为类作用域实现，去掉文件级 static
void WiFi_TX::printVendorIE(const vendor_ie_data_t* ie) {
    if (!ie) return;
    ESP_LOGI(TAG, "IE: ID=0x%02X, Len=%d, OUI: %02X:%02X:%02X, Type=0x%02X",
             ie->element_id, ie->length,
             ie->vendor_oui[0], ie->vendor_oui[1], ie->vendor_oui[2],
             ie->vendor_oui_type);
    uint8_t payload_len = (ie->length >= 4) ? (ie->length - 4) : 0;
    ESP_LOG_BUFFER_HEX(TAG, ie->payload, payload_len > 16 ? 16 : payload_len);
}

// JSON 辅助函数
esp_err_t WiFi_TX::_send_json_response(httpd_req_t *req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t WiFi_TX::_send_error_response(httpd_req_t *req, int code, const char* msg) {
    httpd_resp_set_status(req, code == 401 ? "401 Unauthorized" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf), "{\"error\":%d,\"msg\":\"%s\"}", code, msg);
    return httpd_resp_send(req, err_buf, strlen(err_buf));
}

// 🔹 路由处理器实现
esp_err_t WiFi_TX::_http_get_device_info(httpd_req_t *req) {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_AP, mac);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "project", app_desc->project_name);
    cJSON_AddStringToObject(root, "version", app_desc->version);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac_ap", mac_str);
    cJSON_AddStringToObject(root, "oui", "FA:0B:BC");
    cJSON_AddNumberToObject(root, "oui_type", RID_OUI_TYPE);
    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json_response(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WiFi_TX::_http_get_system_info(httpd_req_t *req) {
    uint32_t uptime_ms = esp_timer_get_time() / 1000 - s_start_time_ms;
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_8BIT);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_sec", uptime_ms / 1000);
    cJSON_AddNumberToObject(root, "free_heap", heap_info.total_free_bytes);
    cJSON_AddNumberToObject(root, "min_free_heap", heap_info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "encode_mode", static_cast<int>(s_encode_mode));
    const char* mode_str = (s_encode_mode == RID_EncodeMode::MODE_GB46750_ONLY) ? "GB46750" :
                          (s_encode_mode == RID_EncodeMode::MODE_RID_ONLY) ? "RID" : "DUAL";
    cJSON_AddStringToObject(root, "encode_mode_str", mode_str);
    cJSON_AddBoolToObject(root, "ap_active", true); // ✅ 修复：替换不存在的 esp_wifi_ap_get_record
    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json_response(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WiFi_TX::_http_get_status(httpd_req_t *req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "counter", Counter);
    cJSON_AddNumberToObject(root, "last_broadcast_ms", esp_timer_get_time() / 1000);
    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json_response(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

// 【新增】GET /api/config 处理器
esp_err_t WiFi_TX::_http_get_config(httpd_req_t *req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "encode_mode", static_cast<int>(s_encode_mode));
    cJSON_AddStringToObject(root, "uas_id", Parameters::get_str(PARAM_UAS_ID));
    cJSON_AddStringToObject(root, "reg_mark", Parameters::get_str(PARAM_REG_MARK));
    cJSON_AddNumberToObject(root, "op_category", Parameters::get_uint8(PARAM_OP_CATEGORY));
    cJSON_AddNumberToObject(root, "ua_class", Parameters::get_uint8(PARAM_UA_CLASS));
    cJSON_AddNumberToObject(root, "wifi_channel", Parameters::get_uint8(PARAM_WIFI_CH));
    
    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json_response(req, resp);
    free((void*)resp); 
    cJSON_Delete(root);
    return ret;
}

// 【增强】POST /api/config 处理器 (支持多参数热更新)
esp_err_t WiFi_TX::_http_post_config(httpd_req_t *req) {
    // 1. Token 认证
    char token_buf[64] = {0};
    size_t token_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (token_len > 0 && token_len < sizeof(token_buf)) {
        httpd_req_get_hdr_value_str(req, "Authorization", token_buf, sizeof(token_buf));
        if (strcmp(token_buf, "Bearer " WEB_AUTH_TOKEN) != 0) return _send_error_response(req, 401, "Invalid token");
    } else { return _send_error_response(req, 401, "Missing Authorization"); }

    // 2. 接收 JSON
    char recv_buf[512];
    int ret = httpd_req_recv(req, recv_buf, sizeof(recv_buf) - 1);
    if (ret <= 0) { if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req); return ESP_FAIL; }
    recv_buf[ret] = '\0';

    cJSON* body = cJSON_Parse(recv_buf);
    if (!body) return _send_error_response(req, 400, "Invalid JSON");

    // 3. 解析并应用配置
    cJSON* item;
    if ((item = cJSON_GetObjectItem(body, "encode_mode")) && cJSON_IsNumber(item)) {
        int new_mode = item->valueint;
        if (new_mode >= 0 && new_mode <= 1) {
            s_encode_mode = static_cast<RID_EncodeMode>(new_mode);
            ESP_LOGI(TAG, "Encode mode changed to: %d", new_mode);
        }
    }
    if ((item = cJSON_GetObjectItem(body, "uas_id")) && cJSON_IsString(item)) {
        Parameters::set_str(PARAM_UAS_ID, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(body, "reg_mark")) && cJSON_IsString(item)) {
        Parameters::set_str(PARAM_REG_MARK, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(body, "op_category")) && cJSON_IsNumber(item)) {
        Parameters::set_uint8(PARAM_OP_CATEGORY, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(body, "ua_class")) && cJSON_IsNumber(item)) {
        Parameters::set_uint8(PARAM_UA_CLASS, item->valueint);
    }

    cJSON_Delete(body);
    return _send_json_response(req, "{\"status\":0,\"msg\":\"Config applied\"}");
}


// 🔹 OTA 升级处理器 (补全实现)
esp_err_t WiFi_TX::_http_ota_update(httpd_req_t *req) {
    char token_buf[64] = {0};
    size_t token_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (token_len > 0 && token_len < sizeof(token_buf)) {
        httpd_req_get_hdr_value_str(req, "Authorization", token_buf, sizeof(token_buf));
        if (strcmp(token_buf, "Bearer " WEB_AUTH_TOKEN) != 0) return _send_error_response(req, 401, "Invalid token");
    }
    size_t content_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (content_len == 0) return _send_error_response(req, 400, "Missing Content-Length");

    esp_ota_handle_t ota_handle;
    const esp_partition_t* ota_partition = esp_ota_get_next_update_partition(nullptr);
    if (!ota_partition) return _send_error_response(req, 500, "No OTA partition");
    if (esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) return _send_error_response(req, 500, "OTA init failed");

    char* buf = (char*)malloc(HTTP_MAX_REQ_LEN);
    if (!buf) { esp_ota_abort(ota_handle); return _send_error_response(req, 500, "No memory"); }
    size_t received = 0;
    int read_len;
    while ((read_len = httpd_req_recv(req, buf, HTTP_MAX_REQ_LEN)) > 0) {
        if (esp_ota_write(ota_handle, buf, read_len) != ESP_OK) { esp_ota_abort(ota_handle); free(buf); return _send_error_response(req, 500, "Write failed"); }
        received += read_len;
    }
    free(buf);

    if (esp_ota_end(ota_handle) != ESP_OK) return _send_error_response(req, 400, "Invalid firmware");
    if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) return _send_error_response(req, 500, "Set boot failed");

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "status", 0); cJSON_AddStringToObject(resp, "msg", "OK. Rebooting in 3s...");
    const char* resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json"); httpd_resp_send(req, resp_str, strlen(resp_str));
    free((void*)resp_str); cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(3000)); esp_restart();
    return ESP_OK;
}

// 🔹 注册/启动/停止 Server
bool WiFi_TX::registerOtaHandler(httpd_handle_t server) {
    if (!server) return false;
    httpd_uri_t uri = {"/ota", HTTP_POST, _http_ota_update, nullptr};
    return httpd_register_uri_handler(server, &uri) == ESP_OK;
}

// 【修改】startWebServer (更新 HTML 页面并注册 GET /api/config)
bool WiFi_TX::startWebServer() {
    if (_http_server) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT; 
    config.max_uri_handlers = 10;
    config.recv_wait_timeout = 10; 
    config.send_wait_timeout = 10; 
    config.lru_purge_enable = true;

    if (httpd_start(&_http_server, &config) != ESP_OK) { 
        ESP_LOGE(TAG, "HTTP start failed"); return false; 
    }

    // ✅ 更新 HTML 导航页
    const httpd_uri_t uri_root = { "/", HTTP_GET, [](httpd_req_t *req) {
        const char* html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>XC-RID</title></head><body>"
                           "<h1>XC-RID Device</h1>"
                           "<h3>GET APIs (No Auth)</h3>"
                           "<ul>"
                           "<li><a href='/api/device'>/api/device</a> (Device Info)</li>"
                           "<li><a href='/api/system'>/api/system</a> (System Info)</li>"
                           "<li><a href='/api/status'>/api/status</a> (Broadcast Status)</li>"
                           "<li><a href='/api/config'>/api/config</a> (Current Config)</li>"
                           "</ul>"
                           "<h3>POST APIs (Auth: Bearer admin123)</h3>"
                           "<ul>"
                           "<li>POST /api/config (JSON body, e.g. {\"encode_mode\": 1})</li>"
                           "<li>POST /ota (Binary firmware)</li>"
                           "</ul>"
                           "</body></html>";
        httpd_resp_set_type(req, "text/html"); 
        return httpd_resp_send(req, html, strlen(html));
    }, nullptr};

    const httpd_uri_t uri_dev = { "/api/device", HTTP_GET, _http_get_device_info, nullptr};
    const httpd_uri_t uri_sys = { "/api/system", HTTP_GET, _http_get_system_info, nullptr};
    const httpd_uri_t uri_sta = { "/api/status", HTTP_GET, _http_get_status, nullptr};
    const httpd_uri_t uri_cfg_get = { "/api/config", HTTP_GET, _http_get_config, nullptr}; // ✅ 新增 GET
    const httpd_uri_t uri_cfg_post = { "/api/config", HTTP_POST, _http_post_config, nullptr};
    const httpd_uri_t uri_ota = { "/ota", HTTP_POST, _http_ota_update, nullptr};

    httpd_register_uri_handler(_http_server, &uri_root);
    httpd_register_uri_handler(_http_server, &uri_dev);
    httpd_register_uri_handler(_http_server, &uri_sys);
    httpd_register_uri_handler(_http_server, &uri_sta);
    httpd_register_uri_handler(_http_server, &uri_cfg_get);
    httpd_register_uri_handler(_http_server, &uri_cfg_post);
    httpd_register_uri_handler(_http_server, &uri_ota);

    ESP_LOGI(TAG, "HTTP Server started on port %d", HTTP_SERVER_PORT);
    return true;
}

void WiFi_TX::stopWebServer() { if (_http_server) { httpd_stop(_http_server); _http_server = nullptr; } }
bool WiFi_TX::isWebServerRunning() const { return _http_server != nullptr; }

// 🔹 init 与 transmit
bool WiFi_TX::init() {
    if (_initialised) return true;
    _initialised = true;
    s_start_time_ms = esp_timer_get_time() / 1000; // ✅ 已包含 esp_timer.h
    ESP_LOGI(TAG, "Initializing Wi-Fi AP...");
    for (int i = 0; i < 6; i++) _mac[i] = (uint8_t)(esp_random() & 0xFF);
    _mac[0] |= 0x02; _mac[0] &= 0xFE;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_wifi_driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg)); s_wifi_driver_inited = true;
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
    wifi_config.ap.max_connection = 4; wifi_config.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 10, 0, 0, 1); IP4_ADDR(&ip_info.gw, 10, 0, 0, 1); IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP Started. SSID: %s, IP: 10.0.0.1", ap_ssid);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(52);
    startWebServer(); // 自动启动统一服务
    return true;
}

bool WiFi_TX::transmit(const RIDData &data) {
    init();
    Counter = (Counter + 1) & 0xFF;
    
    uint8_t payload_buf[256]; // ASTM Message Pack 可能较长，扩大缓冲区
    int payload_len = 0;
    bool need_prefix_counter = false; 

    // 1. 根据模式编码 Payload
    if (s_encode_mode == RID_EncodeMode::MODE_GB46750_ONLY) {
        payload_len = GB46750Encoder::encode(data, payload_buf, sizeof(payload_buf));
        need_prefix_counter = true; 
    } 
    else if (s_encode_mode == RID_EncodeMode::MODE_RID_ONLY) {
        // ✅ 【核心修复】将 RIDData 桥接映射为 ASTM 子结构体
        
        // 1.1 Basic ID
        RIDBasicID basic_id = {};
        basic_id.ua_type = RID_UA_HELICOPTER; // 默认多旋翼/直升机
        basic_id.id_type = RID_ID_SERIAL_NO;
        strncpy(basic_id.uas_id, data.uas_id, sizeof(basic_id.uas_id) - 1);

        // 1.2 Location
        RIDLocation location = {};
        location.status = RID_STATUS_AIRBORNE; 
        location.direction = data.track_deg;
        location.speed_h = data.ground_speed_ms;
        location.speed_v = data.vert_speed_ms;
        location.latitude = data.lat;
        location.longitude = data.lon;
        location.altitude_baro = data.baro_alt_m;
        location.altitude_geo = data.geo_alt_m;
        location.height = data.rel_alt_m;
        location.height_is_above_ground = true;
        location.acc_horiz = RID_ACC_10M;
        location.acc_vert = RID_ACC_10M;
        location.acc_baro = RID_ACC_10M;
        location.acc_speed = RID_ACC_10M;
        location.acc_ts = RID_ACC_0_1NM;
        // ASTM 要求 timestamp 是 10分钟内的秒数 (0-3600)
        location.timestamp_sec = (data.timestamp_ms % 3600000) / 1000.0f; 

        // 1.3 System (Operator Location)
        RIDSystem system = {};
        system.op_loc_type = RIDSystem::TAKEOFF;
        system.op_latitude = data.gcs_lat;
        system.op_longitude = data.gcs_lon;
        system.op_altitude_geo = data.gcs_alt;
        system.category_eu = static_cast<RIDSystem::CategoryEU>(data.op_category);
        system.class_eu = static_cast<RIDSystem::ClassEU>(data.ua_class);
        system.timestamp_full = data.timestamp_ms / 1000;
        system.area_count = 1;
        system.area_radius = 50;
        system.area_ceiling = 120;
        system.area_floor = 0;

        // 1.4 Operator ID
        RIDOperatorID op_id = {};
        op_id.id_type = RIDOperatorID::CAA_ID;
        strncpy(op_id.operator_id, data.reg_mark, sizeof(op_id.operator_id) - 1);

        // 1.5 调用 ASTM 编码器
        payload_len = RID_Encoder::encodePayload(
            payload_buf, Counter, 
            &basic_id, location, &system, &op_id
        );
        need_prefix_counter = false; // ASTM encodePayload 内部已经处理了 counter 和 message pack 头部
    } 
    else {
        ESP_LOGW(TAG, "Invalid mode, fallback to GB46750");
        payload_len = GB46750Encoder::encode(data, payload_buf, sizeof(payload_buf));
        need_prefix_counter = true;
    }

    if (payload_len <= 0) { 
        ESP_LOGE(TAG, "Encoder failed"); 
        return false; 
    }

    // 2. 组装 Vendor IE (单槽位)
    size_t ie_payload_len = need_prefix_counter ? (1 + payload_len) : payload_len;
    size_t ie_size = sizeof(vendor_ie_data_t) + ie_payload_len;
    
    vendor_ie_data_t *ie = static_cast<vendor_ie_data_t *>(malloc(ie_size));
    if (!ie) return false;

    ie->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->vendor_oui[0] = RID_OUI_0; 
    ie->vendor_oui[1] = RID_OUI_1; 
    ie->vendor_oui[2] = RID_OUI_2;
    ie->vendor_oui_type = RID_OUI_TYPE; 
    ie->length = static_cast<uint8_t>(4 + ie_payload_len);
    
    // 3. 填充 Payload
    if (need_prefix_counter) {
        ie->payload[0] = Counter;
        memcpy(ie->payload + 1, payload_buf, payload_len);
    } else {
        // ASTM 模式：encodePayload 返回的已经是完整的 Message Pack (包含头部和所有消息)
        memcpy(ie->payload, payload_buf, payload_len);
    }

    // 调试打印
    if (Counter == 0x01) printVendorIE(ie);

    // 4. 注入 Beacon 和 Probe Response
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    bool ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    
    if (ok) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
        ok = (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) == ESP_OK);
    }

    free(ie);
    return ok;
}

void WiFi_TX::setEncodeMode(RID_EncodeMode mode) { s_encode_mode = mode; }
RID_EncodeMode WiFi_TX::getEncodeMode() const { return s_encode_mode; }