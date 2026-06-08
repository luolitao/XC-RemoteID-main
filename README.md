

# 🚁 XC-RemoteID (GB 46750-2025)

> 基于纯 ESP-IDF 架构的工业级无人机 Remote ID 广播固件，专为 ESP32-S0WD (单核) 极限优化。

## 📖 项目简介

**XC-RemoteID** 是一款符合中国民航局 **GB 46750-2025**（及 ASTM F3411）标准的无人机远程识别（Remote ID）发射器固件。

本项目彻底摒弃了臃肿的 Arduino 框架，采用**纯 ESP-IDF 5.x (Native C/C++)** 架构开发。针对资源极度受限的 **ESP32-S0WD (单核 160MHz, 无 PSRAM)** 芯片进行了“外科手术级”的裁剪与优化。在剔除复杂 Web 服务与经典蓝牙的前提下，实现了极低功耗的 Wi-Fi Beacon 广播、优雅的 NimBLE 现场配网，以及支持 SHA-256 强校验的工业级无线 OTA 升级模块。

## ✨ 核心特性

*   **🚀 纯 ESP-IDF 原生架构**：基于 FreeRTOS 状态机模型，彻底消除 Arduino `loop()` 阻塞与内存碎片，RAM 占用极低。
*   **🛡️ 硬件瑕疵免疫 (eFuse 修复)**：独创“代码级 MAC 覆盖 + Kconfig 底层忽略”双重机制，完美绕过 ESP32-S0WD 常见的 eFuse MAC CRC 校验错误导致的 `abort()` 硬崩溃。
*   **📡 Wi-Fi Beacon 广播**：利用 `esp_wifi_set_vendor_ie` 将国标数据包注入 802.11 Beacon 和 Probe Response 帧，无需设备连接即可被 RemoteID 接收 App 扫描。
*   **📱 极简 BLE 现场配网 (NimBLE)**：开机自动提供 2 分钟蓝牙配网窗口，支持修改 UAS ID、实名登记码及起飞点经纬度。超时自动无缝切换至 Wi-Fi 广播模式，彻底释放 BLE 内存。
*   **☁️ 工业级无线 OTA**：内置极简 HTTP Server，支持纯二进制流推送、ESP-IDF 原生 SHA-256 固件校验与 Rollback (防变砖) 机制。
*   **⚡ 单核芯片极限优化**：精准配置 `CONFIG_FREERTOS_UNICORE`，释放被双核 IPC 占用的宝贵资源，杜绝单核环境下的 Task Watchdog Timeout (TWDT) 与 Brownout 重启。
*   **🔄 动态版本管理**：通过 Python Pre-build 脚本在编译瞬间注入精确到分钟的时间戳，彻底解决增量编译导致的版本号缓存陷阱。

## 🔄 系统状态机流转

设备采用严格的“分时复用”状态机，确保 Wi-Fi 与 BLE 永远不会同时运行，杜绝射频共存冲突与内存溢出：

1. **上电启动**：读取 NVS 状态，标记当前 OTA 分区为 `VALID`（建立防变砖安全港）。
2. **CONFIG 模式 (BLE)**：启动 NimBLE 外设，广播 `XC-RID-CFG`，等待手机写入参数。
   - *分支 A*：收到 Save 指令 -> 写入 NVS -> 硬件重启。
   - *分支 B*：2 分钟超时 -> 优雅关闭 BLE -> 无缝切入 NORMAL 模式。
3. **NORMAL 模式 (Wi-Fi)**：初始化 Wi-Fi AP (隐藏/可见可配) -> 启动 OTA HTTP 服务 -> 启动 1Hz RemoteID 广播循环。

## 🛠️ 硬件与环境要求

*   **主控芯片**：ESP32-S0WD (单核, 4MB Flash) 或标准 ESP32-WROOM-32
*   **开发框架**：ESP-IDF v5.x (通过 PlatformIO 管理)
*   **操作系统**：macOS / Linux / Windows
*   **依赖工具**：`curl` (用于 OTA 推送), Python 3.11 (用于 PlatformIO 及版本注入脚本)

## 🚀 快速开始

### 1. 环境准备
强烈建议使用 Python 3.11 环境运行 PlatformIO，以避免 Python 3.12+ 移除 `distutils` 导致的构建崩溃。
```bash
# 使用 uv 安装 PlatformIO (推荐)
uv tool install platformio --python 3.11
```

### 2. 编译与首次烧录
*首次必须通过串口烧录，以建立 OTA 分区表和 NVS 初始状态。*
```bash
# 清理缓存并编译 (Pre-build 脚本会自动生成最新时间戳)
rm -rf .pio
pio run -e esp32s0wd_mock

# 擦除 Flash 并烧录
pio run -t erase -e esp32s0wd_mock --upload-port /dev/tty.usbserial-XXXX
pio run -e esp32s0wd_mock --target upload --upload-port /dev/tty.usbserial-XXXX
```

## 📱 BLE 配网指南 (nRF Connect)

设备开机后，使用手机打开 **nRF Connect**，扫描并连接名为 **`XC-RID-CFG`** 的设备。展开 `Unknown Service` (UUID: `0000FFE0-...`)，按照下表进行读写：

| UUID | 参数名称 | 数据类型 | 说明 / 示例 |
| :--- | :--- | :--- | :--- |
| `0xFFE1` | UAS ID | String (UTF-8) | 唯一产品识别码 (_max 20字节_) |
| `0xFFE2` | Reg Mark | String (UTF-8) | 实名登记标志 (_max 8字节_) |
| `0xFFE3` | Op Category | Uint8 | 运行类别 (0=未定义, 1=开放, 2=特定, 3=审定) |
| `0xFFE4` | UA Class | Uint8 | 无人机分类 (0=微, 1=轻, 2=小, 3=中, 4=大) |
| `0xFFE5` | Takeoff Lat | Float (4字节) | 起飞点纬度 (例: `23.1429`) |
| `0xFFE6` | Takeoff Lon | Float (4字节) | 起飞点经度 (例: `113.2602`) |
| `0xFFE7` | Takeoff Alt | Float (4字节) | 起飞点海拔 (例: `14.0` 米) |
| **`0xFFE8`** | **Save & Reboot**| **Uint8 / Hex** | **保存并重启** (写入任意值如 `01` 触发) |

## ☁️ OTA 无线升级指南

设备进入 NORMAL 模式后，会自动创建 Wi-Fi 热点（默认 IP 为 `10.0.0.1`）。连接该热点后，即可进行无线升级。

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

1. **eFuse MAC CRC Error 导致 PHY `abort()`**
   * **现象**：部分 ESP32-S0WD 芯片出厂 eFuse MAC 损坏，Wi-Fi 初始化时底层 `esp_phy_load_cal_and_init` 触发 `ESP_ERROR_CHECK` 导致无限重启。
   * **解法**：必须在 `sdkconfig.defaults` 中强制开启 `CONFIG_ESP32_IGNORE_MAC_CRC_ERROR=y`，并在 `app_main` 最早期调用 `esp_base_mac_addr_set()` 覆盖合法 MAC。
2. **ESP-IDF 5.x NimBLE 初始化崩溃 (`LoadProhibited`)**
   * **现象**：在 5.x 中手动调用 `esp_bt_controller_init()` 后再调用 `nimble_port_init()` 会导致底层状态机冲突、内存重复分配，引发空指针崩溃。
   * **解法**：5.x 中 `nimble_port_init()` 已自动包含控制器初始化，**绝对不要**手动调用控制器 API。
3. **NimBLE 广播名称不显示 (`BLE_HS_ENOTSYNCED`)**
   * **现象**：调用 `ble_gap_adv_start` 报错 Not Synced，且手机搜不到设备名称。
   * **解法**：必须使用 `ble_hs_cfg.sync_cb` 注册回调，等待 Host 与 Controller 同步完成后再启动广播；同时必须使用 `ble_hs_adv_fields` 显式将名称打包进 Advertising Payload。
4. **OTA 升级时的 `0x2d` Magic Byte 错误**
   * **现象**：使用 `curl -F` 上传固件时，设备报 `invalid magic byte (expected 0xE9, saw 0x2d)`。
   * **解法**：`-F` 会添加 multipart 表单边界（`-` 的 ASCII 即为 `0x2d`）。ESP32 的 `httpd_req_recv` 需要纯二进制流，必须改用 `curl --data-binary "@file.bin"`。
5. **OTA Rollback (防变砖) 失效陷阱**
   * **现象**：新固件崩溃后，Bootloader 没有回退到旧固件，而是陷入无限死循环。
   * **解法**：旧固件必须调用过 `esp_ota_mark_app_valid_cancel_rollback()` 将自己标记为 `VALID`，才能成为 Bootloader 认可的“安全港”。
6. **单核芯片 Brownout (欠压) 重启**
   * **现象**：蓝牙或 Wi-Fi 初始化瞬间电流飙升，触发硬件级断电复位。
   * **解法**：在 `sdkconfig` 中限制 CPU 频率为 80MHz，关闭经典蓝牙 (BR/EDR)，极限压缩 NimBLE 内存 (`CONFIG_BT_NIMBLE_NVS_PERSIST=n`)。

## 📂 目录结构

```text
XC-RemoteID/
├── src/                      # 扁平化源码目录 (LDF 扫描效率最高)
│   ├── main.cpp              # 纯 ESP-IDF 入口 (状态机主控, OTA 服务)
│   ├── wifi_tx.cpp/h         # Wi-Fi Vendor IE 组包与发射
│   ├── ble_config.cpp/h      # NimBLE GATT 配网服务
│   ├── encoder.cpp/h         # 国标数据包编码算法 (纯 C++)
│   ├── mock_data.cpp/h       # 模拟飞行轨迹生成器 (读取 NVS 坐标)
│   ├── parameters.cpp/h      # NVS 参数持久化存储 (支持 Float/Blob)
│   ├── version.cpp/h         # 版本号管理逻辑
│   └── version_auto.h        # [自动生成] 编译时间戳 (Git 忽略)
├── pre_build_version.py      # Pre-build 脚本 (注入实时编译时间)
├── partitions_ota.csv        # 4MB Flash OTA 专属分区表
├── sdkconfig.defaults        # 核心 Kconfig (单核、MAC忽略、NimBLE优化)
└── platformio.ini            # PlatformIO 构建配置
```

## 📄 许可证

本项目仅供学习研究与合规测试使用。请严格遵守当地民航局关于无人机实名登记与 Remote ID 广播的相关法律法规。
代码遵循 MIT License 开源。