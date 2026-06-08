#pragma once
#include <cstdint>
#include "encoder.h"
#include <esp_http_server.h>
#include <esp_wifi_types.h>  // ✅ 新增：解决 vendor_ie_data_t 未定义

enum class RID_EncodeMode : uint8_t {
    MODE_GB46750_ONLY = 0,
    MODE_RID_ONLY     = 1,
    MODE_DUAL         = 2
};

class WiFi_TX {
public:
    bool init();
    bool transmit(const RIDData &data);
    // ✅ 保持为类的静态成员函数
    static void printVendorIE(const vendor_ie_data_t* ie);    
    void setEncodeMode(RID_EncodeMode mode);
    RID_EncodeMode getEncodeMode() const;
    bool startWebServer();
    void stopWebServer();
    bool isWebServerRunning() const;
    static bool registerOtaHandler(httpd_handle_t server);

private:
    bool _initialised = false;
    uint8_t _mac[6];
    httpd_handle_t _http_server = nullptr;

    // 辅助函数 
    static esp_err_t _send_json_response(httpd_req_t *req, const char* json);
    static esp_err_t _send_error_response(httpd_req_t *req, int code, const char* msg);

    // HTTP 路由处理器
    static esp_err_t _http_get_device_info(httpd_req_t *req);
    static esp_err_t _http_get_system_info(httpd_req_t *req);
    static esp_err_t _http_get_status(httpd_req_t *req);
    static esp_err_t _http_post_config(httpd_req_t *req);
    static esp_err_t _http_ota_update(httpd_req_t *req);  // ✅ 补全缺失声明
};