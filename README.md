
# 🚁 XC-RemoteID (GB 46750-2025)

> 基于纯 ESP-IDF 架构的工业级无人机 Remote ID 广播固件，专为 ESP32-S0WD (单核) 深度优化。

## 📖 项目简介

**XC-RemoteID** 是一款符合中国民航局 **GB 46750-2025**（及 ASTM F3411）标准的无人机远程识别（Remote ID）发射器固件。
本项目彻底摒弃了臃肿的 Arduino 框架，采用**纯 ESP-IDF (Native C/C++)** 架构开发。针对资源受限的 **ESP32-S0WD (单核、无 PSRAM)** 芯片进行了极限优化，在剔除蓝牙和复杂 Web 服务的前提下，实现了极低功耗、极高稳定性的 Wi-Fi Vendor IE 广播，并内置了支持 SHA-256 强校验的工业级无线 OTA 升级模块。

## ✨ 核心特性

*   **🚀 纯 ESP-IDF 原生架构**：基于 FreeRTOS 多任务模型，彻底消除 Arduino `loop()` 阻塞与内存碎片，RAM 占用极低。
*   **🛡️ 硬件瑕疵免疫 (eFuse 损坏修复)**：独创“代码级 MAC 覆盖 + Kconfig 底层忽略”双重机制，完美绕过 ESP32-S0WD 常见的 eFuse MAC CRC 校验错误导致的 `abort()` 硬崩溃。
*   **📡  Wi-Fi Beacon 广播**：利用 `esp_wifi_set_vendor_ie` 将国标数据包注入 802.11 Beacon 和 Probe Response 帧，无需设备连接即可被 RemoteID 接收 App 扫描。
*   **☁️ 工业级无线 OTA**：内置极简 HTTP Server，支持 ESP-IDF 原生 SHA-256 固件校验与 Rollback (防变砖) 机制，支持 `curl` 或 VSCode 一键无线飞升。
*   **⚡ 单核芯片极限优化**：精准配置 `CONFIG_FREERTOS_UNICORE`，释放被双核 IPC 占用的宝贵资源，杜绝单核环境下的 Task Watchdog Timeout (TWDT)。
*   **💾 智能射频校准**：自动管理 `phy_init` 分区，首次启动后进行全量 RF 校准并持久化，后续启动秒开。

## 🛠️ 硬件与环境要求

*   **主控芯片**：ESP32-S0WD (单核 160MHz, 4MB Flash) 或标准 ESP32-WROOM-32
*   **开发框架**：ESP-IDF v5.x (通过 PlatformIO 管理)
*   **操作系统**：macOS / Linux / Windows
*   **依赖工具**：`curl` (用于 OTA 推送), `uv` 或 `pipx` (推荐用于管理 PlatformIO 的 Python 环境)

## 🚀 快速开始

### 1. 环境准备
强烈建议使用 Python 3.11 环境运行 PlatformIO，以避免 Python 3.12+ 移除 `distutils` 导致的 `pkg_resources` 缺失问题。
```bash
# 使用 uv 安装 PlatformIO (推荐)
uv tool install platformio --python 3.11
```

### 2. 克隆与编译
```bash
git clone <your-repo-url> XC-RemoteID
cd XC-RemoteID

# 清理缓存并编译
rm -rf .pio
pio run -e esp32s0wd_mock
```

### 3. 串口烧录 (首次)
*首次必须通过串口烧录，以建立 OTA 分区表和 Wi-Fi 热点环境。*
```bash
pio run -t erase -e esp32s0wd_mock --upload-port /dev/tty.usbserial-XXXX
pio run -e esp32s0wd_mock --target upload --upload-port /dev/tty.usbserial-XXXX
```

## 📡 OTA 无线升级指南

设备启动后，会自动创建一个名为 `XC-RID-XXXXXX` 的 Wi-Fi 热点（默认无密码，IP 为 `10.0.0.1`）。连接该热点后，即可进行无线升级。

### 方式一：终端 curl 一键推送
```bash
# 编译生成新固件
pio run -e esp32s0wd_mock

# 通过纯二进制流推送到设备 (注意：必须使用 --data-binary，不能用 -F)
curl --data-binary "@.pio/build/esp32s0wd_mock/firmware.bin" http://10.0.0.1/update
```

### 方式二：VSCode 侧边栏一键 Upload
在 `platformio.ini` 中已配置自定义上传协议：
```ini
upload_protocol = custom
upload_command = curl -s --data-binary "@$SOURCE" http://10.0.0.1/update
```
只需点击 VSCode 底部的 **➡️ (Upload)** 按钮，PlatformIO 将自动编译并通过 Wi-Fi 完成固件推送与设备重启。

## ⚠️ 核心工程笔记 (踩坑与避坑指南)

本项目在开发过程中跨越了多个 ESP32 底层的“地狱级”陷阱，特此记录以供社区参考：

1.  **eFuse MAC CRC Error 导致 PHY `abort()`**
    *   **现象**：部分 ESP32-S0WD 芯片出厂 eFuse MAC 损坏，Wi-Fi 初始化时底层 `esp_phy_load_cal_and_init` 触发 `ESP_ERROR_CHECK` 导致无限重启。
    *   **解法**：必须在 `sdkconfig.defaults` 中强制开启 `CONFIG_ESP32_IGNORE_MAC_CRC_ERROR=y`，并在 `app_main` 最早期调用 `esp_base_mac_addr_set()` 覆盖合法 MAC。
2.  **单核芯片 (S0WD) 的双核假设崩溃**
    *   **现象**：Arduino 框架和默认 ESP-IDF 配置假设芯片为双核，初始化 APP CPU 时导致单核芯片 Hard Fault。
    *   **解法**：`sdkconfig.defaults` 必须包含 `CONFIG_FREERTOS_UNICORE=y`，且 `platformio.ini` 中需确保没有引入双核专属的 IPC 中断配置。
3.  **Python 3.12+ 环境导致的构建崩溃**
    *   **现象**：ESP-IDF 4.4/5.x 的底层 CMake 脚本依赖 `pkg_resources`，而 Python 3.12 已将其移除，导致 `ModuleNotFoundError`。
    *   **解法**：绝对不要使用系统默认的 Python 3.12+。使用 `uv` 或 `pyenv` 强制指定 Python 3.11 来创建 PlatformIO 虚拟环境。
4.  **OTA 升级时的 `0x2d` Magic Byte 错误**
    *   **现象**：使用 `curl -F` 上传固件时，设备报 `invalid magic byte (expected 0xE9, saw 0x2d)`。
    *   **解法**：`-F` 会添加 multipart 表单边界（`-` 的 ASCII 即为 `0x2d`）。ESP32 的 `httpd_req_recv` 需要纯二进制流，必须改用 `curl --data-binary "@file.bin"`。
5.  **混合框架 (Arduino + ESP-IDF) 的时序黑盒**
    *   **现象**：NVS 初始化失败 (`0x1101`)，Wi-Fi 驱动无法注销。
    *   **解法**：彻底放弃 `framework = arduino, espidf` 混合模式。在纯 ESP-IDF 中手动控制 `nvs_flash_init()` 和 `esp_wifi_init()` 的绝对先后顺序。

## 📂 目录结构

```text
XC-RemoteID/
├── include/                  # 全局头文件
├── src/
│   ├── main.cpp              # 纯 ESP-IDF 入口 (app_main, FreeRTOS 任务)
│   ├── broadcast/
│   │   └── wifi_tx.cpp       # Wi-Fi Vendor IE 组包与发射
│   ├── gb46750/
│   │   └── encoder.cpp       # 国标数据包编码算法 (纯 C++)
│   ├── mock/
│   │   └── mock_data.cpp     # 模拟飞行轨迹生成器
│   └── system/
│       └── parameters.cpp    # NVS 参数持久化存储
├── partitions_ota.csv        # 4MB Flash OTA 专属分区表
├── sdkconfig.defaults        # 核心 Kconfig (单核、MAC忽略、OTA支持)
└── platformio.ini            # PlatformIO 构建配置
```

## 📄 许可证

本项目仅供学习研究与合规测试使用。请严格遵守当地民航局关于无人机实名登记与 Remote ID 广播的相关法律法规。
代码遵循 MIT License 开源。