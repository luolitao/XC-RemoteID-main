/*
 * 模拟飞行数据实现 (Pure ESP-IDF Native Architecture)
 */

#include "mock_data.h"
#include <math.h>
#include "esp_random.h"

// ==========================================
// ESP-IDF 原生头文件 (替代 Arduino 的 millis 和 Serial)
// ==========================================
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "MOCK_DATA";

// 圆心：广州越秀区 (根据您之前的日志调整)
static const double CENTER_LAT =  23.1429;
static const double CENTER_LON = 113.2602;
static const float  RADIUS_M   = 50.0f;
static const float  ALT_GEO    = 54.0f;    // 海拔约4m + 飞行高度50m
static const float  SPEED_MS   = 5.0f;

static const double LAT_PER_M  = 1.0 / 111320.0;
static const double LON_PER_M  = 1.0 / (111320.0 * cos(CENTER_LAT * M_PI / 180.0));

float    MockData::_angle_deg = 0.0f;
uint32_t MockData::_last_ms   = 0;

// ==========================================
// 辅助函数：获取系统毫秒数 (完美替代 millis())
// ==========================================
static inline uint32_t get_millis() {
    // esp_timer_get_time() 返回微秒 (int64_t)，除以 1000 得到毫秒
    // 转换为 uint32_t 后，会在约 49.7 天后溢出回绕，这与 Arduino 的 millis() 行为完全一致
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void MockData::init(RIDData &data)
{
    // 身份信息（测试用，实际由 NVS 参数覆盖）
    snprintf(data.uas_id, sizeof(data.uas_id), "%s", "ESP32MOCKSIMBD1234AB");
    snprintf(data.reg_mark, sizeof(data.reg_mark), "%s", "MOCK0001");

    data.op_category  = GBOpCategory::OPEN;
    data.ua_class     = GBUAClass::LIGHT;
    data.coord_type   = GBCoordType::WGS84;

    // 遥控站：起飞点
    data.gcs_pos_type = GBGCSPosType::TAKEOFF;
    data.gcs_lat      = CENTER_LAT;
    data.gcs_lon      = CENTER_LON;
    data.gcs_alt      = 14.0f;    
    data.gcs_pos_valid = true;

    // 精度（GPS 正常精度）
    data.horiz_acc = GBHorizAcc::LT_10M;
    data.vert_acc  = GBVertAcc::LT_10M;
    data.spd_acc   = GBSpdAcc::LT_1MS;
    data.ts_acc    = GBTsAcc::LTE_100MS;

    data.location_valid = true;
    
    // 替代 millis()
    _last_ms = get_millis(); 
    
    // 替代 Serial.println
    ESP_LOGI(TAG, "[XC-RID] RIDDATA initialized");
}

void MockData::update(RIDData &data)
{
    // 替代 millis()
    const uint32_t now_ms = get_millis();

    // 每 2000ms (2秒) 推进一步 (注意：您原代码写的是 2000，如果是 200ms 请改为 200)
    if (now_ms - _last_ms < 2000) return; 
    
    const float dt = (now_ms - _last_ms) * 0.001f;
    _last_ms = now_ms;

    // 绕圆：角速度 = speed / radius (rad/s)
    const float omega_deg = (SPEED_MS / RADIUS_M) * (180.0f / M_PI);
    _angle_deg += omega_deg * dt;
    if (_angle_deg >= 360.0f) _angle_deg -= 360.0f;

    const float rad = _angle_deg * M_PI / 180.0f;

    // 位置
    data.lat = CENTER_LAT + RADIUS_M * cos(rad) * LAT_PER_M;
    data.lon = CENTER_LON + RADIUS_M * sin(rad) * LON_PER_M;
    data.geo_alt_m  = ALT_GEO;
    data.baro_alt_m = ALT_GEO - 2.0f;   // 气压高度略低
    data.rel_alt_m  = 50.0f;             // 相对起飞点高度

    // 速度：切线方向
    float track = _angle_deg + 90.0f;
    if (track >= 360.0f) track -= 360.0f;
    data.track_deg       = track;
    data.ground_speed_ms = SPEED_MS;
    data.vert_speed_ms   = 0.0f;   // 匀速平飞

    // 运行状态
    data.op_status = GBOpStatus::AIRBORNE;

    // 时间戳（Unix ms，用 ESP32 启动时间近似）
    // 2024-01-01 00:00:00 UTC = 1704067200000 ms
    data.timestamp_ms = 1704067200000ULL + now_ms;

    // 调试日志 (使用 ESP_LOGD，在 release 模式下会自动被编译器优化掉，不占性能)
    ESP_LOGD(TAG, "Mock data updated - Lat: %f, Lon: %f, Alt: %.1fm", 
             data.lat, data.lon, data.geo_alt_m);
}