#include "rid_encoder.h"
#include <cmath>

// 辅助编码函数
static uint8_t encodeDirection(float deg) { return (deg >= 361.0f || deg < 0.0f) ? 255 : static_cast<uint8_t>(roundf(deg * 255.0f / 360.0f)); }
static uint8_t encodeSpeedH(float ms) { return (ms >= 255.0f || ms < 0.0f) ? 255 : ((ms >= 254.25f) ? 254 : static_cast<uint8_t>(roundf(ms))); }
static int8_t encodeSpeedV(float ms) { return (ms >= 63.0f) ? 63 : ((ms <= -63.0f) ? -64 : static_cast<int8_t>(roundf(ms))); }
static int32_t encodeLatLon(double deg, bool is_lat) {
    if (deg == 0.0) return 0;
    double limit = is_lat ? 90.0 : 180.0;
    return (deg < -limit || deg > limit) ? 0 : static_cast<int32_t>(round(deg * 1e7));
}
static uint16_t encodeAltitude(float m) {
    if (m <= -1000.0f) return 0xFFFF;
    if (m >= 11000.0f) return 0xFFFE;
    return static_cast<uint16_t>(roundf((m + 1000.0f) * 2.0f));
}
static uint16_t encodeTimestamp(float sec) { return (sec >= 3600.0f || sec < 0.0f) ? 0xFFFF : static_cast<uint16_t>(roundf(sec * 10.0f)); }

// 单消息编码实现
static void encodeBasicID(uint8_t* out, const RIDBasicID& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_BASIC_ID & 0x0F) << 4);
    out[1] = (static_cast<uint8_t>(in.ua_type) & 0x0F) | ((static_cast<uint8_t>(in.id_type) & 0x0F) << 4);
    strncpy(reinterpret_cast<char*>(out + 2), in.uas_id, 20);
}

static void encodeLocation(uint8_t* out, const RIDLocation& in) {
    memset(out, 0, RID_SINGLE_MSG_SIZE);
    out[0] = (RID_PROTOCOL_VERSION & 0x0F) | ((RID_MSG_LOCATION & 0x0F) << 4);
    out[1] = (static_cast<uint8_t>(in.status) & 0x0F) |
             ((in.height_is_above_ground ? 1 : 0) << 4) |
             ((in.longitude >= 0 ? 1 : 0) << 5) |
             ((in.speed_h >= 0 ? 1 : 0) << 6);
    out[2] = encodeDirection(in.direction);
    out[3] = encodeSpeedH(in.speed_h);
    out[4] = static_cast<uint8_t>(encodeSpeedV(in.speed_v));
    
    int32_t lat = encodeLatLon(in.latitude, true);  memcpy(out + 5, &lat, 4);
    int32_t lon = encodeLatLon(in.longitude, false); memcpy(out + 9, &lon, 4);
    
    uint16_t alt_baro = encodeAltitude(in.altitude_baro); memcpy(out + 13, &alt_baro, 2);
    uint16_t alt_geo  = encodeAltitude(in.altitude_geo);  memcpy(out + 15, &alt_geo, 2);
    uint16_t height   = encodeAltitude(in.height);        memcpy(out + 17, &height, 2);
    
    out[19] = (static_cast<uint8_t>(in.acc_horiz) & 0x0F) | ((static_cast<uint8_t>(in.acc_vert) & 0x0F) << 4);
    out[20] = (static_cast<uint8_t>(in.acc_speed) & 0x0F) | ((static_cast<uint8_t>(in.acc_baro) & 0x0F) << 4);
    
    uint16_t ts = encodeTimestamp(in.timestamp_sec); memcpy(out + 21, &ts, 2);
    out[23] = static_cast<uint8_t>(in.acc_ts) & 0x0F;
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

// 🔑 主编码函数（修复序列点，补全调用）
int RID_Encoder::encodePayload(uint8_t* out_payload, uint8_t counter,
                              const RIDBasicID* basic_id, const RIDLocation& location,
                              const RIDSystem* system, const RIDOperatorID* operator_id)
{
    if (!out_payload) return -1;
    out_payload[0] = counter;

    const uint8_t* messages[RID_MAX_PACK_MESSAGES];
    uint8_t temp_msgs[RID_MAX_PACK_MESSAGES][RID_SINGLE_MSG_SIZE];
    uint8_t msg_count = 0;

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

    if (msg_count == 0) return 1;
    for (uint8_t i = 0; i < msg_count; i++) {
        memcpy(out_payload + 1 + i * RID_SINGLE_MSG_SIZE, messages[i], RID_SINGLE_MSG_SIZE);
    }
    return 1 + msg_count * RID_SINGLE_MSG_SIZE;
}