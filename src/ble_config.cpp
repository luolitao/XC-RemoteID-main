#include "ble_config.h"
#include "parameters.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char* TAG = "BLE_CFG";
static bool s_should_save_and_reboot = false;

// 自定义 Service 和 Characteristic UUIDs
static const ble_uuid16_t gatt_svr_svc_uuid = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t chr_uas_id_uuid   = BLE_UUID16_INIT(0xFFE1);
static const ble_uuid16_t chr_reg_mark_uuid = BLE_UUID16_INIT(0xFFE2);
static const ble_uuid16_t chr_save_uuid     = BLE_UUID16_INIT(0xFFE3);

// GATT 访问回调
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    
    if (uuid == 0xFFE1) { // UAS ID
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
    else if (uuid == 0xFFE2) { // Reg Mark
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
    else if (uuid == 0xFFE3) { // Save & Reboot
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            ESP_LOGW(TAG, "Save command received! Preparing to reboot...");
            Parameters::set_uint8(PARAM_CONFIGURED, 1);
            s_should_save_and_reboot = true;
        }
    }
    return 0;
}

// GATT 服务定义
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = &chr_uas_id_uuid.u,
            .access_cb = gatt_svr_chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        }, {
            .uuid = &chr_reg_mark_uuid.u,
            .access_cb = gatt_svr_chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        }, {
            .uuid = &chr_save_uuid.u,
            .access_cb = gatt_svr_chr_access,
            .flags = BLE_GATT_CHR_F_WRITE,
        }, {
            0, /* No more characteristics in this service */
        } },
    },
    { 0 }, /* No more services */
};

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_config_start() {
    ESP_LOGI(TAG, "Starting BLE Config Server (NimBLE)...");
    s_should_save_and_reboot = false;

    esp_nimble_hci_and_controller_init();
    nimble_port_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    ble_hs_util_ensure_addr(0);
    ble_svc_gap_device_name_set("XC-RID-CFG");

    nimble_port_freertos_init(ble_host_task);
    
    // 开始广播
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    ESP_LOGI(TAG, "BLE Advertising started. Connect via nRF Connect.");
}

void ble_config_stop() {
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    esp_nimble_hci_and_controller_deinit();
    ESP_LOGI(TAG, "BLE Config Server stopped.");
}

bool ble_config_is_done() {
    return s_should_save_and_reboot;
}