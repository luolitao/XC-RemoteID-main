#include "mock_data.h"
#include "parameters.h" 
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h" // 【关键新增】引入 ESP-IDF 高精度定时器

static const char* TAG = "MOCK_DATA";

static const float  RADIUS_M   = 50.0f;
static const float  FLIGHT_ALT = 50.0f; // 相对起飞点高度 50m
static const float  SPEED_MS   = 1.0f;

// 动态计算的系数
static double s_lat_per_m = 1.0 / 111320.0;
static double s_lon_per_m = 1.0 / 111320.0;

float    MockData::_angle_deg = 0.0f;
uint32_t MockData::_last_ms   = 0;

// ==========================================
// 【关键修复】完美替代 Arduino millis()
// ==========================================
static inline uint32_t get_millis() {
    // esp_timer_get_time() 返回微秒，除以 1000 得到毫秒
    // 转换为 uint32_t 后，行为与 Arduino millis() 完全一致（约 49.7 天后溢出回绕）
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void MockData::init(RIDData &data)
{
    // 1. 读取身份参数
    const char* uas_id = Parameters::get_str(PARAM_UAS_ID);
    const char* reg_mark = Parameters::get_str(PARAM_REG_MARK);
    snprintf(data.uas_id, sizeof(data.uas_id), "%s", strlen(uas_id) > 0 ? uas_id : "ESP32MOCKSIMBD1234AB");
    snprintf(data.reg_mark, sizeof(data.reg_mark), "%s", strlen(reg_mark) > 0 ? reg_mark : "MOCK0001");

    // 2. 读取分类参数
    data.op_category  = static_cast<GBOpCategory>(Parameters::get_uint8(PARAM_OP_CATEGORY));
    data.ua_class     = static_cast<GBUAClass>(Parameters::get_uint8(PARAM_UA_CLASS));
    data.coord_type   = GBCoordType::WGS84;

    // 3. 读取起飞点 (遥控站) 坐标
    data.gcs_pos_type = GBGCSPosType::TAKEOFF;
    data.gcs_lat      = Parameters::get_float(PARAM_GCS_LAT, 23.1429f);
    data.gcs_lon      = Parameters::get_float(PARAM_GCS_LON, 113.2602f);
    data.gcs_alt      = Parameters::get_float(PARAM_GCS_ALT, 14.0f);
    data.gcs_pos_valid = true;

    // 动态计算经度补偿系数 (因为不同纬度下，1米对应的经度差是不同的)
    s_lon_per_m = 1.0 / (111320.0 * cos(data.gcs_lat * M_PI / 180.0));

    ESP_LOGI(TAG, "Using UAS ID: %s", data.uas_id);
    ESP_LOGI(TAG, "Using Takeoff Pos: Lat=%f, Lon=%f, Alt=%f", data.gcs_lat, data.gcs_lon, data.gcs_alt);

    // 精度设置
    data.horiz_acc = GBHorizAcc::LT_10M;
    data.vert_acc  = GBVertAcc::LT_10M;
    data.spd_acc   = GBSpdAcc::LT_1MS;
    data.ts_acc    = GBTsAcc::LTE_100MS;

    data.location_valid = true;
 
   // 【使用修复后的时间函数】
    _last_ms = get_millis(); 
    ESP_LOGI(TAG, "[XC-RID] RIDDATA initialized");
}

void MockData::update(RIDData &data)
{
    // 【使用修复后的时间函数】
    const uint32_t now_ms = get_millis();
    
    // 每 2000ms (2秒) 推进一步 (5 Hz 更新，1Hz 广播)
    if (now_ms - _last_ms < 2000) return; 
    
    const float dt = (now_ms - _last_ms) * 0.001f;
    _last_ms = now_ms;

   // 绕圆：角速度 = speed / radius (rad/s)
    const float omega_deg = (SPEED_MS / RADIUS_M) * (180.0f / M_PI);
    _angle_deg += omega_deg * dt;
    if (_angle_deg >= 360.0f) _angle_deg -= 360.0f;

    const float rad = _angle_deg * M_PI / 180.0f;

    // 使用动态读取的起飞点坐标作为圆心
    data.lat = data.gcs_lat + RADIUS_M * cos(rad) * s_lat_per_m;
    data.lon = data.gcs_lon + RADIUS_M * sin(rad) * s_lon_per_m;
    
    data.geo_alt_m  = data.gcs_alt + FLIGHT_ALT; 
    data.baro_alt_m = data.geo_alt_m - 2.0f;   
    data.rel_alt_m  = FLIGHT_ALT;             

       // 速度：切线方向
    float track = _angle_deg + 90.0f;
    if (track >= 360.0f) track -= 360.0f;
    data.track_deg       = track;
    data.ground_speed_ms = SPEED_MS;
    data.vert_speed_ms   = 0.0f;   

   // 运行状态
    data.op_status = GBOpStatus::AIRBORNE;
    
    // 时间戳（Unix ms）
    // 注意：ESP32 没有 RTC 电池，重启后系统时间是 1970 年。
    // 这里用固定基准时间 + 启动后的运行毫秒数来模拟一个“看起来合理”的递增时间戳。
    data.timestamp_ms = 1704067200000ULL + now_ms;
}