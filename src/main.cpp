/*
XC-RemoteID - GB 46750 / ASTM F3411 compliant Remote ID firmware
Main entry point (BLE Config + WiFi Broadcast + Unified Web Server + OTA)
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"      // ✅ 新增：确保 esp_ota_* 函数可见
#include "esp_ota_ops.h"        // ✅ 显式包含
#include "esp_app_format.h"
#include "parameters.h"
#include "ble_config.h"
#include "wifi_tx.h"
#include "mock_data.h"
#include "version.h"
// 【新增】调试开关：设为 1 跳过蓝牙配网，直接进 Wi-Fi 模式
#define SKIP_BLE_CONFIG 1 

static const char* TAG = "MAIN";
static WiFi_TX wifi_tx;
RIDData mock_rid_data{}; // 确保没有 static 关键字，使其成为全局可见

// 阶段 1: 配网模式
static void run_config_mode() {
    ESP_LOGW(TAG, ">>> ENTERING CONFIG MODE (BLE Only, 2min Timeout) <<<");
    ble_config_start();
    TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = (120 * 1000) / portTICK_PERIOD_MS; 
    while (1) {
        if (ble_config_is_done()) {
            ESP_LOGI(TAG, "Configuration saved via BLE. Rebooting...");
            ble_config_stop();
            vTaskDelay(500 / portTICK_PERIOD_MS);
            esp_restart(); 
        }
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_LOGW(TAG, "Config timeout. Switching to Normal mode...");
            ble_config_stop();
            vTaskDelay(200 / portTICK_PERIOD_MS);
            return;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// 阶段 2: 运行模式
static void run_normal_mode() {
    ESP_LOGI(TAG, ">>> ENTERING NORMAL MODE (WiFi Broadcast + Unified Web) <<<");
    if (!wifi_tx.init()) {
        ESP_LOGE(TAG, "WiFi TX init failed! System halted.");
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
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

// 程序入口
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  XC-RemoteID Firmware v%s", get_full_version());
    ESP_LOGI(TAG, "========================================\n");

    // OTA 安全港
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state != ESP_OTA_IMG_VALID) {
            ESP_LOGW(TAG, "Marking partition as VALID...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // NVS 初始化
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    Parameters::init();

    // 【修改】状态机流转
#if SKIP_BLE_CONFIG
    ESP_LOGW(TAG, ">>> SKIP_BLE_CONFIG is ON. Bypassing BLE mode! <<<");
#else
    run_config_mode();  // 正常流程：开机先进入 2 分钟 BLE 配网
#endif

    run_normal_mode(); // 进入 Wi-Fi 广播 + OTA + Web API 监听
}