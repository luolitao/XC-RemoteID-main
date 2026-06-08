#include "ble_config.h"
#include "parameters.h"
#include "esp_log.h"
#include "esp_bt.h" // 保留以防需要底层宏
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char* TAG = "BLE_CFG";
static bool s_should_save_and_reboot = false;

// ... 保留原有的 include 和 TAG ...

// 【更新】UUID 定义 (Save 顺延至 0xFFE8)
static const ble_uuid16_t gatt_svr_svc_uuid = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t chr_uas_id_uuid   = BLE_UUID16_INIT(0xFFE1);
static const ble_uuid16_t chr_reg_mark_uuid = BLE_UUID16_INIT(0xFFE2);
static const ble_uuid16_t chr_op_cat_uuid   = BLE_UUID16_INIT(0xFFE3);
static const ble_uuid16_t chr_ua_cls_uuid   = BLE_UUID16_INIT(0xFFE4);
static const ble_uuid16_t chr_gcs_lat_uuid  = BLE_UUID16_INIT(0xFFE5);
static const ble_uuid16_t chr_gcs_lon_uuid  = BLE_UUID16_INIT(0xFFE6);
static const ble_uuid16_t chr_gcs_alt_uuid  = BLE_UUID16_INIT(0xFFE7);
static const ble_uuid16_t chr_save_uuid     = BLE_UUID16_INIT(0xFFE8); 

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    
    // --- 字符串类型 (UAS ID / Reg Mark) ---
    if (uuid == 0xFFE1) { 
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char* val = Parameters::get_str(PARAM_UAS_ID);
            os_mbuf_append(ctxt->om, val, strlen(val));
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len <= 20) {
                char buf[21] = {0};
                os_mbuf_copydata(ctxt->om, 0, len, buf);
                Parameters::set_str(PARAM_UAS_ID, buf);
                ESP_LOGI(TAG, "UAS ID updated: %s", buf);
            }
        }
    } 
    else if (uuid == 0xFFE2) { 
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char* val = Parameters::get_str(PARAM_REG_MARK);
            os_mbuf_append(ctxt->om, val, strlen(val));
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len <= 8) {
                char buf[9] = {0};
                os_mbuf_copydata(ctxt->om, 0, len, buf);
                Parameters::set_str(PARAM_REG_MARK, buf);
                ESP_LOGI(TAG, "Reg Mark updated: %s", buf);
            }
        }
    }
    // --- Uint8 类型 (Op Cat / UA Class) ---
    else if (uuid == 0xFFE3 || uuid == 0xFFE4) {
        const char* key = (uuid == 0xFFE3) ? PARAM_OP_CATEGORY : PARAM_UA_CLASS;
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t val = Parameters::get_uint8(key);
            os_mbuf_append(ctxt->om, &val, 1);
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (OS_MBUF_PKTLEN(ctxt->om) == 1) {
                uint8_t val;
                os_mbuf_copydata(ctxt->om, 0, 1, &val);
                Parameters::set_uint8(key, val);
                ESP_LOGI(TAG, "Param 0x%04X updated: %d", uuid, val);
            }
        }
    }
    // --- Float 类型 (Lat / Lon / Alt) ---
    else if (uuid == 0xFFE5 || uuid == 0xFFE6 || uuid == 0xFFE7) {
        const char* key = (uuid == 0xFFE5) ? PARAM_GCS_LAT : (uuid == 0xFFE6 ? PARAM_GCS_LON : PARAM_GCS_ALT);
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            float val = Parameters::get_float(key, 0.0f);
            os_mbuf_append(ctxt->om, &val, sizeof(float));
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (OS_MBUF_PKTLEN(ctxt->om) == sizeof(float)) {
                float val;
                os_mbuf_copydata(ctxt->om, 0, sizeof(float), &val);
                Parameters::set_float(key, val);
                ESP_LOGI(TAG, "Param 0x%04X updated: %f", uuid, val);
            }
        }
    }
    // --- Save & Reboot ---
    else if (uuid == 0xFFE8) { 
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            ESP_LOGW(TAG, "Save command received! Preparing to reboot...");
            Parameters::set_uint8(PARAM_CONFIGURED, 1);
            s_should_save_and_reboot = true;
        }
    }
    return 0;
}

// 【更新】GATT 服务定义 (注册所有新特征值)
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) { 
            { .uuid = &chr_uas_id_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_reg_mark_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_op_cat_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_ua_cls_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_gcs_lat_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_gcs_lon_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_gcs_alt_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_save_uuid.u, .access_cb = gatt_svr_chr_access, .flags = BLE_GATT_CHR_F_WRITE },
            { 0 }, 
        },
    },
    { 0 }, 
};

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ==========================================
// Host 与 Controller 同步完成后的回调
// ==========================================
static void ble_on_sync(void) {
    ESP_LOGI(TAG, "Host and Controller synced. Configuring advertising data...");
    
    // 【关键修复】显式构建广播数据包 (Advertising Payload)
    struct ble_hs_adv_fields fields = {0};
    
    // 1. 设置 Flags (必须：表明设备是可发现的，且不支持经典蓝牙)
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    // 2. 设置设备名称 (让手机在列表里直接看到 "XC-RID-CFG")
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1; // 1 表示这是完整名称
    
    // 3. 设置发射功率 (可选：让手机显示信号强度 RSSI 参考)
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;

    // 将构建好的数据应用到广播中
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        return;
    }

    // 4. 设置广播参数
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // 允许连接
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // 通用可发现模式
    
    // 优化广播频率 (100ms 广播一次，让手机更快搜到)
    adv_params.itvl_min = 160; // 160 * 0.625ms = 100ms
    adv_params.itvl_max = 160; 
    
    // 5. 启动广播
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE Advertising started with name: %s", name);
    }
}

void ble_config_start() {
    ESP_LOGI(TAG, "Starting BLE Config Server (NimBLE ESP-IDF 5.x)...");
    s_should_save_and_reboot = false;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    // 【关键修复】注册 Sync 回调，确保同步完成后再启动广播
    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    ble_hs_util_ensure_addr(0);
    ble_svc_gap_device_name_set("XC-RID-CFG");

    // 启动 Host 任务 (它会在后台触发 ble_on_sync)
    nimble_port_freertos_init(ble_host_task);
    
    // 【移除】不要在这里直接调用 ble_gap_adv_start()！
}

void ble_config_stop() {
    ble_gap_adv_stop();
    nimble_port_stop();
    
    // nimble_port_deinit 会自动反初始化底层控制器
    nimble_port_deinit(); 
    
    ESP_LOGI(TAG, "BLE Config Server stopped.");
}

bool ble_config_is_done() {
    return s_should_save_and_reboot;
}
