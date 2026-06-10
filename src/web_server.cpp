#include "web_server.h"
#include "wifi_tx.h"
#include "parameters.h"
#include "mock_data.h"
#include "version.h"

#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <esp_wifi.h>       // ✅ 新增：提供 esp_wifi_get_config, esp_wifi_get_mac, esp_wifi_ap_get_sta_list

static const char* TAG = "WEB_SRV";
#define HTTP_SERVER_PORT 80
#define HTTP_MAX_REQ_LEN 4096
#define WEB_AUTH_TOKEN "admin123"

extern RIDData mock_rid_data; // 引用 main.cpp 中的全局变量
httpd_handle_t WebServer::_server = nullptr;

// ==========================================
// 辅助函数
// ==========================================
esp_err_t WebServer::_send_json(httpd_req_t *req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t WebServer::_send_error(httpd_req_t *req, int code, const char* msg) {
    httpd_resp_set_status(req, code == 401 ? "401 Unauthorized" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf), "{\"error\":%d,\"msg\":\"%s\"}", code, msg);
    return httpd_resp_send(req, err_buf, strlen(err_buf));
}

// ==========================================
// HTTP Handlers (全量增强字段)
// ==========================================
esp_err_t WebServer::_handle_root(httpd_req_t *req) {
    const char* html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>XC-RID</title></head><body>"
                       "<h1>XC-RID Device</h1>"
                       "<h3>GET APIs (No Auth)</h3><ul>"
                       "<li><a href='/api/device'>/api/device</a> (Hardware Info)</li>"
                       "<li><a href='/api/system'>/api/system</a> (System & OTA)</li>"
                       "<li><a href='/api/status'>/api/status</a> (Broadcast Stats)</li>"
                       "<li><a href='/api/config'>/api/config</a> (Current Config)</li></ul>"
                       "<h3>POST APIs (Auth: Bearer admin123)</h3><ul>"
                       "<li>POST /api/config (JSON)</li>"
                       "<li>POST /ota (Binary)</li></ul>"
                       "</body></html>";
    httpd_resp_set_type(req, "text/html"); 
    return httpd_resp_send(req, html, strlen(html));
}

esp_err_t WebServer::_handle_device_info(httpd_req_t *req) {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    const char* model_str = "Unknown ESP32";
    switch(chip_info.model) {
        case CHIP_ESP32:   model_str = "ESP32"; break;
        case CHIP_ESP32S2: model_str = "ESP32-S2"; break;
        case CHIP_ESP32S3: model_str = "ESP32-S3"; break;
        default: break;
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    uint32_t total_ram_kb = (heap_info.total_free_bytes + heap_info.total_allocated_bytes) / 1024;

    uint8_t wifi_mac[6], bt_mac[6];
    esp_read_mac(wifi_mac, ESP_MAC_WIFI_SOFTAP);
    esp_read_mac(bt_mac, ESP_MAC_BT);
    char w_mac[18], b_mac[18];
    snprintf(w_mac, sizeof(w_mac), "%02X:%02X:%02X:%02X:%02X:%02X", wifi_mac[0], wifi_mac[1], wifi_mac[2], wifi_mac[3], wifi_mac[4], wifi_mac[5]);
    snprintf(b_mac, sizeof(b_mac), "%02X:%02X:%02X:%02X:%02X:%02X", bt_mac[0], bt_mac[1], bt_mac[2], bt_mac[3], bt_mac[4], bt_mac[5]);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "project", app_desc->project_name);
    cJSON_AddStringToObject(root, "app_version", get_full_version());
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(root, "chip_model", model_str);
    cJSON_AddNumberToObject(root, "chip_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "flash_size_mb", flash_size / (1024.0f * 1024.0f));
    cJSON_AddNumberToObject(root, "ram_size_kb", total_ram_kb);
    cJSON_AddStringToObject(root, "wifi_mac", w_mac);
    cJSON_AddStringToObject(root, "ble_mac", b_mac);
    cJSON_AddStringToObject(root, "vendor_oui", "FA:0B:BC");
    cJSON_AddNumberToObject(root, "oui_type", 0x0D);

    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::_handle_system_info(httpd_req_t *req) {
    BroadcastStats stats = WiFi_TX::getStats();
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_8BIT);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_sec", stats.uptime_ms / 1000);
    cJSON_AddNumberToObject(root, "free_heap", heap_info.total_free_bytes);
    cJSON_AddNumberToObject(root, "min_free_heap", heap_info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "encode_mode", static_cast<int>(WiFi_TX::getEncodeMode()));
    
    const char* mode_str = (WiFi_TX::getEncodeMode() == RID_EncodeMode::MODE_GB46750_ONLY) ? "GB46750" :
                           (WiFi_TX::getEncodeMode() == RID_EncodeMode::MODE_RID_ONLY) ? "ASTM_RID" : "DUAL";
    cJSON_AddStringToObject(root, "encode_mode_str", mode_str);
    cJSON_AddStringToObject(root, "app_version", get_full_version());
    
    const esp_partition_t* part = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "running_partition", part ? part->label : "unknown");

    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::_handle_status(httpd_req_t *req) {
    BroadcastStats stats = WiFi_TX::getStats();
    cJSON* root = cJSON_CreateObject();
    
    // 1. 广播状态 (嵌套)
    cJSON* broadcast = cJSON_CreateObject();
    cJSON_AddNumberToObject(broadcast, "counter", stats.counter);
    cJSON_AddNumberToObject(broadcast, "total_success", stats.total_success);
    cJSON_AddNumberToObject(broadcast, "total_fail", stats.total_fail);
    cJSON_AddNumberToObject(broadcast, "last_broadcast_ms", stats.last_broadcast_ms);
    cJSON_AddNumberToObject(broadcast, "interval_ms", 1000);
    const char* mode_str = (WiFi_TX::getEncodeMode() == RID_EncodeMode::MODE_GB46750_ONLY) ? "GB46750" : "ASTM_RID";
    cJSON_AddStringToObject(broadcast, "protocol", mode_str);
    cJSON_AddItemToObject(root, "broadcast", broadcast);

    // 2. Wi-Fi AP 状态 (嵌套)
    cJSON* wifi_ap = cJSON_CreateObject();
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK) {
        cJSON_AddStringToObject(wifi_ap, "ssid", (char*)conf.ap.ssid);
        cJSON_AddNumberToObject(wifi_ap, "channel", conf.ap.channel);
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(wifi_ap, "mac", mac_str);
    }
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        cJSON_AddNumberToObject(wifi_ap, "connected_stations", sta_list.num);
    } else {
        cJSON_AddNumberToObject(wifi_ap, "connected_stations", 0);
    }
    cJSON_AddItemToObject(root, "wifi_ap", wifi_ap);

    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::_handle_get_config(httpd_req_t *req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "encode_mode", static_cast<int>(WiFi_TX::getEncodeMode()));
    
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    
    // UAS ID Fallback
    const char* uas_id = Parameters::get_str(PARAM_UAS_ID);
    if (strlen(uas_id) == 0) {
        char def[21]; snprintf(def, sizeof(def), "ESP32-%02X%02X%02X", mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(root, "uas_id", def);
    } else { cJSON_AddStringToObject(root, "uas_id", uas_id); }

    // Reg Mark Fallback
    const char* reg_mark = Parameters::get_str(PARAM_REG_MARK);
    if (strlen(reg_mark) == 0) {
        char def[9]; snprintf(def, sizeof(def), "RID-%02X%02X", mac[4], mac[5]);
        cJSON_AddStringToObject(root, "reg_mark", def);
    } else { cJSON_AddStringToObject(root, "reg_mark", reg_mark); }

    cJSON_AddNumberToObject(root, "op_category", Parameters::get_uint8(PARAM_OP_CATEGORY));
    cJSON_AddNumberToObject(root, "ua_class", Parameters::get_uint8(PARAM_UA_CLASS));
    cJSON_AddNumberToObject(root, "wifi_channel", Parameters::get_uint8(PARAM_WIFI_CH));
    cJSON_AddNumberToObject(root, "takeoff_lat", Parameters::get_float(PARAM_GCS_LAT, 0));
    cJSON_AddNumberToObject(root, "takeoff_lon", Parameters::get_float(PARAM_GCS_LON, 0));
    cJSON_AddNumberToObject(root, "takeoff_alt", Parameters::get_float(PARAM_GCS_ALT, 0));
    cJSON_AddNumberToObject(root, "flight_speed", Parameters::get_float(PARAM_FLIGHT_SPEED, 5.0));
    cJSON_AddNumberToObject(root, "orbit_radius", Parameters::get_float(PARAM_ORBIT_RADIUS, 50.0));

    const char* resp = cJSON_PrintUnformatted(root);
    esp_err_t ret = _send_json(req, resp);
    free((void*)resp); cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::_handle_post_config(httpd_req_t *req) {
    char token_buf[64] = {0};
    size_t token_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (token_len > 0 && token_len < sizeof(token_buf)) {
        httpd_req_get_hdr_value_str(req, "Authorization", token_buf, sizeof(token_buf));
        if (strcmp(token_buf, "Bearer " WEB_AUTH_TOKEN) != 0) return _send_error(req, 401, "Invalid token");
    } else { return _send_error(req, 401, "Missing Authorization"); }

    char recv_buf[512];
    int ret = httpd_req_recv(req, recv_buf, sizeof(recv_buf) - 1);
    if (ret <= 0) return _send_error(req, 400, "Recv failed");
    recv_buf[ret] = '\0';

    cJSON* body = cJSON_Parse(recv_buf);
    if (!body) return _send_error(req, 400, "Invalid JSON");

    cJSON* item;
    if ((item = cJSON_GetObjectItem(body, "encode_mode")) && cJSON_IsNumber(item)) {
        RID_EncodeMode new_mode = static_cast<RID_EncodeMode>(item->valueint);
        WiFi_TX::setEncodeMode(new_mode);
        Parameters::set_uint8(PARAM_ENCODE_MODE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(body, "uas_id")) && cJSON_IsString(item)) Parameters::set_str(PARAM_UAS_ID, item->valuestring);
    if ((item = cJSON_GetObjectItem(body, "reg_mark")) && cJSON_IsString(item)) Parameters::set_str(PARAM_REG_MARK, item->valuestring);
    if ((item = cJSON_GetObjectItem(body, "op_category")) && cJSON_IsNumber(item)) Parameters::set_uint8(PARAM_OP_CATEGORY, item->valueint);
    if ((item = cJSON_GetObjectItem(body, "ua_class")) && cJSON_IsNumber(item)) Parameters::set_uint8(PARAM_UA_CLASS, item->valueint);
    if ((item = cJSON_GetObjectItem(body, "takeoff_lat")) && cJSON_IsNumber(item)) Parameters::set_float(PARAM_GCS_LAT, item->valuedouble);
    if ((item = cJSON_GetObjectItem(body, "takeoff_lon")) && cJSON_IsNumber(item)) Parameters::set_float(PARAM_GCS_LON, item->valuedouble);
    if ((item = cJSON_GetObjectItem(body, "takeoff_alt")) && cJSON_IsNumber(item)) Parameters::set_float(PARAM_GCS_ALT, item->valuedouble);
    if ((item = cJSON_GetObjectItem(body, "flight_speed")) && cJSON_IsNumber(item)) Parameters::set_float(PARAM_FLIGHT_SPEED, item->valuedouble);
    if ((item = cJSON_GetObjectItem(body, "orbit_radius")) && cJSON_IsNumber(item)) Parameters::set_float(PARAM_ORBIT_RADIUS, item->valuedouble);
    
    MockData::reload(mock_rid_data); // 热更新生效
    cJSON_Delete(body);
    return _send_json(req, "{\"status\":0,\"msg\":\"Config applied & hot-reloaded\"}");
}

esp_err_t WebServer::_handle_ota(httpd_req_t *req) {
    char token_buf[64] = {0};
    size_t token_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (token_len > 0 && token_len < sizeof(token_buf)) {
        httpd_req_get_hdr_value_str(req, "Authorization", token_buf, sizeof(token_buf));
        if (strcmp(token_buf, "Bearer " WEB_AUTH_TOKEN) != 0) return _send_error(req, 401, "Invalid token");
    }

    esp_ota_handle_t ota_handle;
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) 
        return _send_error(req, 500, "OTA init failed");

    char* buf = (char*)malloc(HTTP_MAX_REQ_LEN);
    int read_len;
    while ((read_len = httpd_req_recv(req, buf, HTTP_MAX_REQ_LEN)) > 0) {
        if (esp_ota_write(ota_handle, buf, read_len) != ESP_OK) {
            esp_ota_abort(ota_handle); free(buf); return _send_error(req, 500, "Write failed");
        }
    }
    free(buf);

    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) 
        return _send_error(req, 400, "Invalid firmware");

    _send_json(req, "{\"status\":0,\"msg\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    esp_restart();
    return ESP_OK;
}

// ==========================================
// 生命周期管理
// ==========================================
bool WebServer::start() {
    if (_server) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

    if (httpd_start(&_server, &config) != ESP_OK) return false;

    httpd_uri_t uris[] = {
        {"/", HTTP_GET, _handle_root, nullptr},
        {"/api/device", HTTP_GET, _handle_device_info, nullptr},
        {"/api/system", HTTP_GET, _handle_system_info, nullptr},
        {"/api/status", HTTP_GET, _handle_status, nullptr},
        {"/api/config", HTTP_GET, _handle_get_config, nullptr},
        {"/api/config", HTTP_POST, _handle_post_config, nullptr},
        {"/ota", HTTP_POST, _handle_ota, nullptr}
    };
    for (auto& uri : uris) httpd_register_uri_handler(_server, &uri);
    
    ESP_LOGI(TAG, "HTTP Server started on port %d", HTTP_SERVER_PORT);
    return true;
}

void WebServer::stop() { if (_server) { httpd_stop(_server); _server = nullptr; } }
bool WebServer::isRunning() { return _server != nullptr; }