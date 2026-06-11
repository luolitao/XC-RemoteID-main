#include "rid_encoder.h"
#include <cmath>
#include <cstring>

// ==========================================
// 辅助编码函数 (严格遵循 ASTM F3411 规范)
// ==========================================

// 航向：1度/LSB。0-179为东(EW=0)，180-359为西(EW=1，值减180)。255=无效
static uint8_t encodeDirection(float deg, bool& is_west) {
    if (deg < 0.0f || deg >= 360.0f) {
        is_west = false;
        return 255; // Invalid
    }
    uint16_t dir = static_cast<uint16_t>(roundf(deg));
    if (dir >= 360) dir -= 360;
    
    if (dir >= 180) {
        is_west = true;
        return static_cast<uint8_t>(dir - 180); 
    } else {
        is_west = false;
        return static_cast<uint8_t>(dir);       
    }
}

// 水平速度：0.25 m/s per LSB, 0~254.25 m/s, 255=Invalid
static uint8_t encodeSpeedH(float ms) {
    if (ms < 0.0f || ms > 254.25f) return 255; 
    return static_cast<uint8_t>(roundf(ms * 4.0f));
}

// 垂直速度：0.25 m/s per LSB, -31.75~+31.75 m/s, 127(0x7F)=Invalid
static int8_t encodeSpeedV(float ms) {
    if (ms < -31.75f || ms > 31.75f) return 127; 
    return static_cast<int8_t>(roundf(ms * 4.0f));
}

// 经纬度：10^-7 度 per LSB
static int32_t encodeLatLon(double deg, bool is_lat) {
    if (deg == 0.0) return 0; // 0 表示未知
    double limit = is_lat ? 90.0 : 180.0;
    return (deg < -limit || deg > limit) ? 0 : static_cast<int32_t>(round(deg * 1e7));
}

// 高度：0.5 m per LSB, 偏移 -1000m
static uint16_t encodeAltitude(float m) {
    if (m <= -1000.0f) return 0xFFFF; // Invalid
    if (m >= 11000.0f) return 0xFFFE; // Invalid
    return static_cast<uint16_t>(roundf((m + 1000.0f) * 2.0f));
}

// 时间戳：0.1s (100ms) per LSB, 0~3600s, 0xFFFF=Invalid
static uint16_t encodeTimestamp(float sec) { 
    if (sec >= 3600.0f || sec < 0.0f) return 0xFFFF; 
    return static_cast<uint16_t>(roundf(sec * 10.0f)); 
}

// ==========================================
// 单消息编码实现 (Location Message - Type 1)
// ==========================================
static void encodeLocation(uint8_t* out, const RIDLocation& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_LOCATION & 0x0F) << 4); // 0x11
    
    bool is_west = false;
    uint8_t dir_byte = encodeDirection(in.direction, is_west);
    uint8_t speed_mult = 0; // 0 = 正常分辨率 (0.25m/s)
    
    // Byte 1: Status(4) | HeightType(1) | EW_Dir(1) | SpeedMult(1) | Reserved(1)
    out[1] = (static_cast<uint8_t>(in.status) & 0x0F) |
             ((in.height_is_above_ground ? 1 : 0) << 4) |
             ((is_west ? 1 : 0) << 5) |             // ✅ 修复：基于航向判断东西向
             ((speed_mult & 0x01) << 6);
             
    out[2] = dir_byte;                              // ✅ 修复：1度/LSB
    out[3] = encodeSpeedH(in.speed_h);              // ✅ 修复：0.25m/s 分辨率
    out[4] = static_cast<uint8_t>(encodeSpeedV(in.speed_v)); // ✅ 修复：0.25m/s 分辨率
    
    int32_t lat = encodeLatLon(in.latitude, true);  memcpy(out + 5, &lat, 4);
    int32_t lon = encodeLatLon(in.longitude, false); memcpy(out + 9, &lon, 4);
    
    uint16_t alt_baro = encodeAltitude(in.altitude_baro); memcpy(out + 13, &alt_baro, 2);
    uint16_t alt_geo  = encodeAltitude(in.altitude_geo);  memcpy(out + 15, &alt_geo, 2);
    uint16_t height   = encodeAltitude(in.height);        memcpy(out + 17, &height, 2);
    
    // Byte 19: Horiz Acc (低4位) | Vert Acc (高4位)
    out[19] = (static_cast<uint8_t>(in.acc_horiz) & 0x0F) | ((static_cast<uint8_t>(in.acc_vert) & 0x0F) << 4);
    
    // Byte 20: Baro Acc (低4位) | Speed Acc (高4位) 
    // ✅ 修复：原代码将 Baro 和 Speed 的高低 4 位写反了，导致解析器校验失败
    out[20] = (static_cast<uint8_t>(in.acc_baro) & 0x0F) | ((static_cast<uint8_t>(in.acc_speed) & 0x0F) << 4);
    
    uint16_t ts = encodeTimestamp(in.timestamp_sec); memcpy(out + 21, &ts, 2);
    
    // Byte 23: TS Acc (低4位) | Reserved (高4位)
    out[23] = static_cast<uint8_t>(in.acc_ts) & 0x0F;
}

// ... (下方的 encodeSystem, encodeOperatorID, encodePayload 保持不变) ...
// 单消息编码实现
static void encodeBasicID(uint8_t* out, const RIDBasicID& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_BASIC_ID & 0x0F) << 4);
    out[1] = (static_cast<uint8_t>(in.ua_type) & 0x0F) | ((static_cast<uint8_t>(in.id_type) & 0x0F) << 4);
    strncpy(reinterpret_cast<char*>(out + 2), in.uas_id, 20);
}

static void encodeSystem(uint8_t* out, const RIDSystem& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_SYSTEM & 0x0F) << 4);
    out[1] = (static_cast<uint8_t>(in.op_loc_type) & 0x03) | ((static_cast<uint8_t>(in.category_eu) & 0x07) << 2);
    int32_t lat = encodeLatLon(in.op_latitude, true);  memcpy(out + 2, &lat, 4);
    int32_t lon = encodeLatLon(in.op_longitude, false); memcpy(out + 6, &lon, 4);
    memcpy(out + 10, &in.area_count, 2);
    out[12] = in.area_radius;
    uint16_t c = static_cast<uint16_t>(in.area_ceiling * 2.0f); memcpy(out + 13, &c, 2);
    uint16_t f = static_cast<uint16_t>(in.area_floor * 2.0f);   memcpy(out + 15, &f, 2);
    out[17] = (static_cast<uint8_t>(in.category_eu) & 0x0F) | ((static_cast<uint8_t>(in.class_eu) & 0x0F) << 4);
    uint16_t alt = encodeAltitude(in.op_altitude_geo); memcpy(out + 18, &alt, 2);
    memcpy(out + 20, &in.timestamp_full, 4);
}

static void encodeOperatorID(uint8_t* out, const RIDOperatorID& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_OPERATOR_ID & 0x0F) << 4);
    out[1] = static_cast<uint8_t>(in.id_type) & 0x0F;
    strncpy(reinterpret_cast<char*>(out + 2), in.operator_id, 20);
}

// 🔑 主编码函数（修复 ASTM Message Pack 头部格式）
int RID_Encoder::encodePayload(uint8_t* out_payload, uint8_t counter,
                               const RIDBasicID* basic_id, const RIDLocation& location,
                               const RIDSystem* system, const RIDOperatorID* operator_id)
{
    if (!out_payload) return -1;
    
    const uint8_t* messages[RID_MAX_PACK_MESSAGES];
    uint8_t temp_msgs[RID_MAX_PACK_MESSAGES][RID_SINGLE_MSG_SIZE];
    uint8_t msg_count = 0;
    
    // 1. 组装单条消息
    if (basic_id) {
        encodeBasicID(temp_msgs[msg_count], *basic_id);
        messages[msg_count] = temp_msgs[msg_count];
        msg_count++;
    }
    
    encodeLocation(temp_msgs[msg_count], location); // Location 必选
    messages[msg_count] = temp_msgs[msg_count];
    msg_count++;
    
    if (system) {
        encodeSystem(temp_msgs[msg_count], *system);
        messages[msg_count] = temp_msgs[msg_count];
        msg_count++;
    }
    
    if (operator_id) {
        encodeOperatorID(temp_msgs[msg_count], *operator_id);
        messages[msg_count] = temp_msgs[msg_count];
        msg_count++;
    }
    
    if (msg_count == 0) return 0;
    
    // ✅ 【核心修复】构建标准的 ASTM F3411 Message Pack 头部
    // Byte 0: Message Type (0xF) << 4 | Protocol Version (0x1) = 0xF1
    out_payload[0] = (RID_MSG_PACK << 4) | (RID_PROTOCOL_VERSION & 0x0F); 
    
    // Byte 1: Single Message Count (包内消息数量)
    out_payload[1] = msg_count;
    
    // 2. 从 Byte 2 开始依次写入各个 25 字节的 Single Message
    for (uint8_t i = 0; i < msg_count; i++) {
        memcpy(out_payload + 2 + i * RID_SINGLE_MSG_SIZE, messages[i], RID_SINGLE_MSG_SIZE);
    }
    
    // 3. 返回总长度：2 字节头部 + N * 25 字节消息
    return 2 + msg_count * RID_SINGLE_MSG_SIZE;
}