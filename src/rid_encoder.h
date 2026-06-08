#pragma once
#include <cstdint>
#include <cstring>

#define RID_PROTOCOL_VERSION    0x1
#define RID_SINGLE_MSG_SIZE     25
#define RID_MAX_PACK_MESSAGES   9

// 消息类型
enum RIDMessageType : uint8_t {
    RID_MSG_BASIC_ID = 0x0, RID_MSG_LOCATION = 0x1, RID_MSG_AUTH = 0x2,
    RID_MSG_SELF_ID = 0x3, RID_MSG_SYSTEM = 0x4, RID_MSG_OPERATOR_ID = 0x5, RID_MSG_PACK = 0xF
};

// 无人机类型
enum RIDUAType : uint8_t {
    RID_UA_NONE = 0, RID_UA_AEROPLANE, RID_UA_HELICOPTER = 3, RID_UA_GYROPLANE,
    RID_UA_HYBRID_LIFT, RID_UA_ORNITHOPTER, RID_UA_GLIDER, RID_UA_KITE,
    RID_UA_FREE_BALLOON, RID_UA_CAPTIVE_BALLOON, RID_UA_AIRSHIP, RID_UA_FREE_FALL,
    RID_UA_ROCKET, RID_UA_TETHERED_POWERED, RID_UA_GROUND_OBSTACLE, RID_UA_OTHER
};

// ID类型
enum RIDIDType : uint8_t {
    RID_ID_NONE = 0, RID_ID_SERIAL_NO, RID_ID_CAA_REG, RID_ID_UTM_ASSIGNED,
    RID_ID_SPECIFIC_SESSION, RID_ID_REGISTRATION, RID_ID_OTHER = 15
};

// 飞行状态
enum RIDStatus : uint8_t {
    RID_STATUS_UNDECLARED = 0, RID_STATUS_GROUND, RID_STATUS_AIRBORNE,
    RID_STATUS_EMERGENCY = 14, RID_STATUS_REMOTE_ID_FAILURE = 15
};

// 精度等级
enum RIDAccuracy : uint8_t {
    RID_ACC_UNKNOWN = 0, RID_ACC_10NM, RID_ACC_4NM, RID_ACC_2NM,
    RID_ACC_1NM, RID_ACC_0_5NM, RID_ACC_0_3NM, RID_ACC_0_1NM,
    RID_ACC_0_05NM = 8, RID_ACC_30M, RID_ACC_10M, RID_ACC_3M, RID_ACC_1M
};

// 数据结构
struct RIDBasicID {
    RIDUAType ua_type;
    RIDIDType id_type;
    char uas_id[21];
};

struct RIDLocation {
    RIDStatus status;
    float direction, speed_h, speed_v;
    double latitude, longitude;
    float altitude_baro, altitude_geo, height;
    bool height_is_above_ground;
    RIDAccuracy acc_horiz, acc_vert, acc_baro, acc_speed, acc_ts;
    float timestamp_sec;
};

struct RIDSystem {
    enum LocType : uint8_t { TAKEOFF = 0, LIVE_GNSS = 1, FIXED = 2 } op_loc_type;
    double op_latitude, op_longitude;
    uint16_t area_count; uint8_t area_radius;
    uint16_t area_ceiling, area_floor;
    enum CategoryEU : uint8_t { EU_CAT_NONE=0, EU_CAT_OPEN=1, EU_CAT_SPECIFIC=2, EU_CAT_CERTIFIED=3 } category_eu;
    enum ClassEU : uint8_t { EU_CLASS_NONE=0, EU_CLASS_0=1, EU_CLASS_1, EU_CLASS_2, EU_CLASS_3, EU_CLASS_4, EU_CLASS_5, EU_CLASS_6 } class_eu;
    float op_altitude_geo;
    uint32_t timestamp_full;
};

struct RIDOperatorID {
    enum IDType : uint8_t { CAA_ID = 0, UTM_ID = 1 } id_type;
    char operator_id[21];
};

// 编码器类声明
class RID_Encoder {
public:
    static int encodePayload(uint8_t* out_payload, uint8_t counter,
                            const RIDBasicID* basic_id, const RIDLocation& location,
                            const RIDSystem* system, const RIDOperatorID* operator_id);
};