/*
 * 参数存储实现（基于 ESP-IDF 原生 NVS）
 * 彻底移除 Arduino Preferences 库，直接使用底层 nvs_flash API
 */

#include "parameters.h"

// ==========================================
// ESP-IDF NVS 头文件
// ==========================================
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "PARAM";
#define NVS_NAMESPACE "xc-rid"

// ==========================================
// 辅助函数：安全打开 NVS 句柄
// ==========================================
static nvs_handle_t open_nvs(nvs_open_mode mode) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return 0; // 返回 0 表示失败
    }
    return handle;
}

// ==========================================
// 初始化与状态检查
// ==========================================
void Parameters::init()
{
    // 1. 确保 NVS 分区已初始化 (双重保险，防止 app_main 中遗漏)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupted or version mismatch, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2. 检查是否已配置，若未配置则加载默认值
    if (!is_configured()) {
        ESP_LOGI(TAG, "Device not configured, loading defaults...");
        load_defaults();
    } else {
        ESP_LOGI(TAG, "Configuration loaded from NVS successfully.");
    }
}

bool Parameters::is_configured()
{
    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (!handle) return false;

    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(handle, PARAM_CONFIGURED, &val);
    nvs_close(handle);

    // 只有当键存在且值为 1 时才认为已配置
    return (err == ESP_OK && val == 1);
}

void Parameters::load_defaults()
{
    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (!handle) return;

    // 写入默认值 (注意：字符串需要以 \0 结尾，C++ 字面量自带)
    nvs_set_str(handle, PARAM_UAS_ID, "");
    nvs_set_str(handle, PARAM_REG_MARK, "");
    nvs_set_u8(handle, PARAM_OP_CATEGORY, 1);   // 开放类
    nvs_set_u8(handle, PARAM_UA_CLASS, 1);      // 轻型
    nvs_set_u32(handle, PARAM_BAUDRATE, 115200);
    nvs_set_u8(handle, PARAM_WIFI_CH, 6);
    
    // 【关键】不写 PARAM_CONFIGURED，保持未配置状态
    
    nvs_commit(handle); // 必须 commit 才会写入 Flash
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Default parameters loaded and committed.");
}

// ==========================================
// 读取接口 (Getters)
// ==========================================
const char *Parameters::get_str(const char *key)
{
    static char buf[64]; // 静态缓冲区，避免每次调用分配内存
    memset(buf, 0, sizeof(buf));
    
    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (!handle) return buf;

    size_t required_size = sizeof(buf);
    esp_err_t err = nvs_get_str(handle, key, buf, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_str '%s' failed: %s", key, esp_err_to_name(err));
    }
    
    nvs_close(handle);
    return buf;
}

uint8_t Parameters::get_uint8(const char *key)
{
    uint8_t val = 0;
    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (!handle) return 0;

    nvs_get_u8(handle, key, &val);
    nvs_close(handle);
    return val;
}

uint32_t Parameters::get_uint32(const char *key)
{
    uint32_t val = 0;
    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (!handle) return 0;

    nvs_get_u32(handle, key, &val);
    nvs_close(handle);
    return val;
}

uint32_t Parameters::get_baudrate()
{
    uint32_t val = 115200; // 默认值
    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (!handle) return val;

    esp_err_t err = nvs_get_u32(handle, PARAM_BAUDRATE, &val);
    if (err != ESP_OK) {
        val = 115200; // 如果读取失败或不存在，强制返回默认值
    }
    nvs_close(handle);
    return val;
}

// ==========================================
// 写入接口 (Setters)
// ==========================================
void Parameters::set_str(const char *key, const char *val)
{
    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (!handle) return;

    nvs_set_str(handle, key, val);
    nvs_commit(handle);
    nvs_close(handle);
}

void Parameters::set_uint8(const char *key, uint8_t val)
{
    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (!handle) return;

    nvs_set_u8(handle, key, val);
    nvs_commit(handle);
    nvs_close(handle);
}

void Parameters::set_uint32(const char *key, uint32_t val)
{
    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (!handle) return;

    nvs_set_u32(handle, key, val);
    nvs_commit(handle);
    nvs_close(handle);
}

// ==========================================
// 恢复出厂设置
// ==========================================
void Parameters::factory_reset()
{
    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (!handle) return;

    ESP_LOGW(TAG, "Factory reset: erasing all data in namespace '%s'", NVS_NAMESPACE);
    nvs_erase_all(handle); // 清空当前 namespace 下的所有键值对
    nvs_commit(handle);
    nvs_close(handle);
    
    // 重新加载默认值
    load_defaults();
}