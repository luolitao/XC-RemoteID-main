#pragma once
#include <esp_http_server.h>

class WebServer {
public:
    static bool start();
    static void stop();
    static bool isRunning();

private:
    static httpd_handle_t _server;
    
    // 辅助函数
    static esp_err_t _send_json(httpd_req_t *req, const char* json);
    static esp_err_t _send_error(httpd_req_t *req, int code, const char* msg);
    
    // HTTP Handlers
    static esp_err_t _handle_root(httpd_req_t *req);
    static esp_err_t _handle_device_info(httpd_req_t *req);
    static esp_err_t _handle_system_info(httpd_req_t *req);
    static esp_err_t _handle_status(httpd_req_t *req);
    static esp_err_t _handle_get_config(httpd_req_t *req);
    static esp_err_t _handle_post_config(httpd_req_t *req);
    static esp_err_t _handle_ota(httpd_req_t *req);
};