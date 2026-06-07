/*
 * XC-RemoteID - GB 46750-2025 compliant Remote ID firmware
 * Main entry point (WiFi-Only + Minimal OTA Support)
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

#include "parameters.h"
#include "wifi_tx.h"
#include "mock_data.h"

static const char* TAG = "MAIN";
static const char* OTA_TAG = "OTA";

static WiFi_TX      wifi_tx;
static RIDData      mock_rid_data{};

// ==========================================
// OTA 全局状态
// ==========================================
static httpd_handle_t ota_server = NULL;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

// ==========================================
// 工业级 OTA HTTP 处理函数 (含版本跟踪与严格校验)
// ==========================================
static esp_err_t ota_update_handler(httpd_req_t *req) {
    char buf[1024];
    int ret;
    esp_err_t err;

    // 1. 获取当前运行固件的版本信息 (用于日志记录和对比)
    const esp_app_desc_t *current_app = esp_app_get_description();
    ESP_LOGI(OTA_TAG, "Current firmware version: %s", current_app->version);
    ESP_LOGI(OTA_TAG, "Current firmware compile time: %s", current_app->date);

    // 2. 初始化 OTA
    if (ota_handle == 0) {
        update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition == NULL) {
            ESP_LOGE(OTA_TAG, "No update partition found");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        
        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(OTA_TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ESP_LOGI(OTA_TAG, "OTA Begin. Target partition: %s", update_partition->label);
    }

    // 3. 循环接收数据块并写入 Flash
    int total_received = 0;
    while ((ret = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        total_received += ret;
        if (esp_ota_write(ota_handle, buf, ret) != ESP_OK) {
            ESP_LOGE(OTA_TAG, "esp_ota_write failed");
            esp_ota_abort(ota_handle);
            ota_handle = 0;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    // 4. 处理传输中断
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        ESP_LOGE(OTA_TAG, "OTA Timeout after receiving %d bytes", total_received);
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }

    // 5. 【核心】传输完成，结束 OTA 并触发底层 SHA-256 校验
    ESP_LOGI(OTA_TAG, "Received %d bytes. Running SHA-256 validation...", total_received);
    err = esp_ota_end(ota_handle);
    ota_handle = 0; // 立即重置句柄

    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(OTA_TAG, "Image validation failed! SHA-256 hash mismatch.");
            httpd_resp_send(req, "Error: Firmware validation failed (Hash mismatch)", HTTPD_RESP_USE_STRLEN);
        } else {
            ESP_LOGE(OTA_TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
        }
        return ESP_FAIL;
    }

    // 6. 校验通过，设置启动分区
    err = esp_ota_set_boot_partition(update_partition);
    if (err == ESP_OK) {
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        ESP_LOGI(OTA_TAG, "Validation SUCCESS! Next boot partition: %s", boot->label);
        ESP_LOGI(OTA_TAG, "Rebooting in 2 seconds...");
        
        httpd_resp_send(req, "OTA Success! Firmware validated. Rebooting...", HTTPD_RESP_USE_STRLEN);
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
    }
    
    return ESP_OK;
}

// ==========================================
// 启动极简 OTA 服务
// ==========================================
static void start_ota_service() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;       // 增加栈大小防止写入时溢出
    config.max_uri_handlers = 1;    // 仅允许 1 个路由，极致节省资源
    config.lru_purge_enable = true; 

    if (httpd_start(&ota_server, &config) == ESP_OK) {
        httpd_uri_t uri_update = {
            .uri      = "/update",
            .method   = HTTP_POST,
            .handler  = ota_update_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(ota_server, &uri_update);
        ESP_LOGI(OTA_TAG, "OTA Server started. POST firmware to http://10.0.0.1/update");
    } else {
        ESP_LOGE(OTA_TAG, "Failed to start OTA Server");
    }
}

// ==========================================
// 辅助函数：修复 S0WD 的 MAC 地址
// ==========================================
static void set_custom_mac_address() {
    uint8_t custom_mac[6] = {0x24, 0x0A, 0xC4, 0x12, 0x34, 0x56};
    esp_err_t err = esp_base_mac_addr_set(custom_mac);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MAC address overridden successfully!");
    } else {
        ESP_LOGE(TAG, "Failed to override MAC: %d", err);
    }
}

// ==========================================
// FreeRTOS 任务：WiFi 广播循环
// ==========================================
void wifi_broadcast_task(void *pvParameters) {
    ESP_LOGI(TAG, "Entering WiFi-Only Broadcast Mode...");
    
    bool wifi_ok = wifi_tx.init();
    if (!wifi_ok) {
        ESP_LOGE(TAG, "WiFi TX init failed! System halted.");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "WiFi TX initialized successfully.");

    // 【关键】在 Wi-Fi 稳定后，启动 OTA 服务
    start_ota_service();

    // 标记当前固件为有效，取消回滚倒计时 (开发阶段必备)
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "App marked as valid, rollback disabled for development.");

    MockData::init(mock_rid_data);
    ESP_LOGI(TAG, "Mock data initialized. Starting 1Hz WiFi broadcast loop...");

    TickType_t last_broadcast_tick = xTaskGetTickCount();
    const TickType_t broadcast_interval = 1000 / portTICK_PERIOD_MS;

    while (1) {
        MockData::update(mock_rid_data);

        if (xTaskGetTickCount() - last_broadcast_tick >= broadcast_interval) {
            last_broadcast_tick = xTaskGetTickCount();
            wifi_tx.transmit(mock_rid_data);
        }
        
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==========================================
// ESP-IDF 程序入口
// ==========================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "\n=== [XC-RID] System Boot (WiFi + OTA Mode) ===");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    set_custom_mac_address();
    Parameters::init();

    xTaskCreate(wifi_broadcast_task, "wifi_task", 8192, NULL, 5, NULL);
}