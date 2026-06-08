#include "version.h"
#include "version_auto.h" // 【新增】引入脚本动态生成的头文件
#include <stdio.h>

static char full_version[64];
static bool is_init = false;

const char* get_full_version() {
    if (is_init) return full_version;

#if APP_IS_DEV_BUILD
    // 【修改】使用脚本注入的精确宏，彻底告别增量编译缓存陷阱
    snprintf(full_version, sizeof(full_version), "v%d.%d.%d-dev.%04d%02d%02d.%02d%02d",
             APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH,
             AUTO_BUILD_YEAR, AUTO_BUILD_MONTH, AUTO_BUILD_DAY, 
             AUTO_BUILD_HOUR, AUTO_BUILD_MIN);
#else
    snprintf(full_version, sizeof(full_version), "v%d.%d.%d",
             APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH);
#endif

    is_init = true;
    return full_version;
}