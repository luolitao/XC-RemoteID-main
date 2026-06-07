/*
 * XC-RemoteID - GB 46750-2025 compliant Remote ID firmware
 * Main entry point (Ultra-Minimal, WiFi ONLY, Pure ESP-IDF)
 */
#include <stdio.h>
#include <string.h>

// ==========================================
// ESP-IDF 原生头文件
// ==========================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"

// ==========================================
// 业务模块头文件 (已移除 BLE 和 WebServer)
// ==========================================
#include "parameters.h"
#include "broadcast/wifi_tx.h"
#include "mock_data.h"

static const char* TAG = "MAIN";

// ==========================================
// 全局实例
// ==========================================
static WiFi_TX      wifi_tx;
static RIDData      mock_rid_data{};

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
    
    // 1. 初始化 Wi-Fi 广播接口
    bool wifi_ok = wifi_tx.init();
    if (!wifi_ok) {
        ESP_LOGE(TAG, "WiFi TX init failed! System halted.");
        vTaskDelete(NULL); // 初始化失败则删除任务
        return;
    }
    ESP_LOGI(TAG, "WiFi TX initialized successfully.");

    // 2. 初始化 Mock 数据
    MockData::init(mock_rid_data);
    ESP_LOGI(TAG, "Mock data initialized. Starting 1Hz WiFi broadcast loop...");

    TickType_t last_broadcast_tick = xTaskGetTickCount();
    const TickType_t broadcast_interval = 1000 / portTICK_PERIOD_MS; // 1Hz

    while (1) {
        // 更新模拟数据 (位置、速度等)
        MockData::update(mock_rid_data);

        // 定时广播 (1Hz)
        if (xTaskGetTickCount() - last_broadcast_tick >= broadcast_interval) {
            last_broadcast_tick = xTaskGetTickCount();
            wifi_tx.transmit(mock_rid_data);
        }
        
        // 让出 CPU，防止触发单核芯片的 Task Watchdog
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==========================================
// ESP-IDF 程序入口
// ==========================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "\n=== [XC-RID] System Boot (WiFi-Only Mode) ===");

    // 1. 修复 NVS 和 MAC (针对 ESP32-S0WD 硬件瑕疵)
    ESP_LOGI(TAG, "-> Initializing NVS...");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    set_custom_mac_address();

    // 2. 加载参数 (内部会自动写入默认值并标记为已配置)
    Parameters::init();

    // 3. 启动 Wi-Fi 广播任务 (由于去掉了 BLE，8192 的栈空间现在极其充裕)
    xTaskCreate(wifi_broadcast_task, "wifi_task", 8192, NULL, 5, NULL);
}