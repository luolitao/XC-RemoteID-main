import datetime
import os

# 1. 获取当前系统时间
now = datetime.datetime.now()

# 2. 提取时间元素
build_year = now.year
build_month = now.month
build_day = now.day
build_hour = now.hour
build_min = now.minute

# 3. 生成 C/C++ 头文件内容
header_content = f"""#pragma once
// ========================================================
// Auto-generated file by pre_build_version.py. 
// DO NOT EDIT MANUALLY.
// ========================================================
#define AUTO_BUILD_YEAR  {build_year}
#define AUTO_BUILD_MONTH {build_month}
#define AUTO_BUILD_DAY   {build_day}
#define AUTO_BUILD_HOUR  {build_hour}
#define AUTO_BUILD_MIN   {build_min}
"""

# 4. 将内容写入 src/version_auto.h
output_path = os.path.join("src", "version_auto.h")

# 优化：只有当内容真正发生变化时才写入文件，避免触发不必要的全局重编译
try:
    with open(output_path, "r") as f:
        if f.read() == header_content:
            print(f"[Version Script] Timestamp unchanged, skipping write.")
            exit(0)
except FileNotFoundError:
    pass

with open(output_path, "w") as f:
    f.write(header_content)
    
print(f"[Version Script] Generated fresh timestamp: {now.strftime('%Y-%m-%d %H:%M')}")