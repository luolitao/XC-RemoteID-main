/*
 * XC-RemoteID - GB 46750-2025 compliant Remote ID firmware
 * Main entry point (BLE Config + WiFi Broadcast + OTA)
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
#include "ble_config.h" 
#include "wifi_tx.h"
#include "mock_data.h"
#include "version.h" 

static const char* TAG = "MAIN";
static const char* OTA_TAG = "OTA";

static WiFi_TX wifi_tx;
static RIDData mock_rid_data{};

// ==========================================
// OTA 全局状态与处理函数
// ==========================================
static httpd_handle_t ota_server = NULL;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

static esp_err_t ota_update_handler(httpd_req_t *req) {
    char buf[1024];
    int ret;
    esp_err_t err;

    const esp_app_desc_t *current_app = esp_app_get_description();
    ESP_LOGI(OTA_TAG, "Current firmware version: %s", get_full_version()); // 使用我们自定义的版本号
    ESP_LOGI(OTA_TAG, "Receiving OTA data...");

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

    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        ESP_LOGE(OTA_TAG, "OTA Timeout after receiving %d bytes", total_received);
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }

    ESP_LOGI(OTA_TAG, "Received %d bytes. Running SHA-256 validation...", total_received);
    err = esp_ota_end(ota_handle);
    ota_handle = 0; 

    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(OTA_TAG, "Image validation failed! SHA-256 hash mismatch.");
            httpd_resp_send(req, "Error: Firmware validation failed", HTTPD_RESP_USE_STRLEN);
        } else {
            ESP_LOGE(OTA_TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
        }
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err == ESP_OK) {
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        ESP_LOGI(OTA_TAG, "Validation SUCCESS! Next boot partition: %s", boot->label);
        ESP_LOGI(OTA_TAG, "Rebooting in 2 seconds...");
        
        httpd_resp_send(req, "OTA Success! Rebooting...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
    }
    
    return ESP_OK;
}

static void start_ota_service() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 1;
    config.lru_purge_enable = true; 

    if (httpd_start(&ota_server, &config) == ESP_OK) {
        httpd_uri_t uri_update = {
            .uri      = "/update",
            .method   = HTTP_POST,
            .handler  = ota_update_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(ota_server, &uri_update);
        ESP_LOGI(OTA_TAG, "OTA Server started. POST binary to http://10.0.0.1/update");
    } else {
        ESP_LOGE(OTA_TAG, "Failed to start OTA Server");
    }
}

// ==========================================
// 辅助函数
// ==========================================
static void set_custom_mac_address() {
    uint8_t custom_mac[6] = {0x24, 0x0A, 0xC4, 0x12, 0x34, 0x56};
    esp_base_mac_addr_set(custom_mac);
}

// ==========================================
// 阶段 1: 配网模式 (BLE + 2分钟超时)
// ==========================================
static void run_config_mode() {
    ESP_LOGW(TAG, ">>> ENTERING CONFIG MODE (BLE Only, 2min Timeout) <<<");
    ble_config_start();

    TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = (120 * 1000) / portTICK_PERIOD_MS; 

    while (1) {
        if (ble_config_is_done()) {
            ESP_LOGI(TAG, "Configuration saved via BLE. Rebooting to apply...");
            ble_config_stop();
            vTaskDelay(500 / portTICK_PERIOD_MS);
            esp_restart(); 
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_LOGW(TAG, "Config mode timeout (2 mins). Switching to Normal mode...");
            ble_config_stop();
            vTaskDelay(200 / portTICK_PERIOD_MS);
            return; // 超时不重启，无缝进入 Wi-Fi 模式
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ==========================================
// 阶段 2: 运行模式 (Wi-Fi 广播 + OTA)
// ==========================================
static void run_normal_mode() {
    ESP_LOGI(TAG, ">>> ENTERING NORMAL MODE (WiFi Broadcast + OTA) <<<");
    
    bool wifi_ok = wifi_tx.init();
    if (!wifi_ok) {
        ESP_LOGE(TAG, "WiFi TX init failed! System halted.");
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    // 【关键修复】在 Wi-Fi 启动后，启动 OTA HTTP 服务
    start_ota_service();

    MockData::init(mock_rid_data);
    TickType_t last_tick = xTaskGetTickCount();

    while (1) {
        MockData::update(mock_rid_data);
        if (xTaskGetTickCount() - last_tick >= 1000 / portTICK_PERIOD_MS) {
            last_tick = xTaskGetTickCount();
            wifi_tx.transmit(mock_rid_data);
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==========================================
// 程序入口 (状态机主控)
// ==========================================
extern "C" void app_main(void) {
    // 1. 打印版本横幅
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  XC-RemoteID Firmware");
    ESP_LOGI(TAG, "  Version: %s", get_full_version());
    ESP_LOGI(TAG, "========================================");

    // 2. OTA 安全港确认机制 (防变砖)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state != ESP_OTA_IMG_VALID) {
            ESP_LOGW(TAG, "Marking current partition as VALID to enable future rollbacks.");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // 3. 初始化 NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 4. 修复 MAC 并加载参数
    set_custom_mac_address();
    Parameters::init();

    // 5. 状态机流转
    run_config_mode();  // 开机先进入 2 分钟 BLE 配网
    run_normal_mode();  // 超时或保存后，进入 Wi-Fi 广播 + OTA 监听
}