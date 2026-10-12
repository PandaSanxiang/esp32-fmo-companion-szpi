# 立创实战派 ESP32-S3 · FMO 伴侣固件（适配版）

> 本仓库为 **FMO 伴侣固件**（[esp32-fmo-companion](https://github.com/zhaozhengde/esp32-fmo-companion)，原作者 BI8SIG）在 **立创实战派 ESP32-S3 开发板** 上的直接刷写与适配版本。
> 本项目旨在将 FMO 固件直接刷入立创开发板 **实战派 ESP32-S3**，并完成显示屏、触摸屏、用户按键等外设的完整适配。
>
> 2026-10-12 由 **BI9CPS** 调整修改。

---

## 目录

- [项目简介](#项目简介)
- [功能特性](#功能特性)
- [硬件说明（立创实战派 ESP32-S3）](#硬件说明立创实战派-esp32-s3)
- [GPIO 引脚分配](#gpio-引脚分配)
- [本适配版相对原版的改动](#本适配版相对原版的改动)
- [刷写环境搭建（详细步骤）](#刷写环境搭建详细步骤)
- [编译固件](#编译固件)
- [烧录固件](#烧录固件)
- [首次使用与配置](#首次使用与配置)
- [用户按键静音功能](#用户按键静音功能)
- [故障排除（常见问题）](#故障排除常见问题)
- [软件架构与目录结构](#软件架构与目录结构)
- [上游功能说明](#上游功能说明)
- [版权与致谢](#版权与致谢)
- [许可证](#许可证)

---

## 项目简介

**FMO（NFM over Internet，互联网模拟通联）** 是由 **BG5ESN** 开发的互联网对讲系统，通过互联网承载 NFM 模拟对讲体验，服务广大业余无线电爱好者。

**FMO 伴侣** 是 FMO 主机的辅助终端：通过 WiFi 连接 FMO 主机，经 WebSocket 获取音频、事件、站点与 QSO 信息，以图形界面呈现通联呼号、通联状态、当前站点、QSO 数量、WiFi/电池状态等信息。

本项目将 FMO 伴侣固件（WireGuard 变体）**直接刷入立创实战派 ESP32-S3 开发板**，并针对该开发板的硬件特性完成了：

- 屏幕显示适配（esp_lcd 官方路径 + PCA9557 片选时序修正）；
- 触摸屏适配（FT6336，ESP-IDF 5.x I2C 兼容补丁）；
- 触摸坐标方向校准；
- 用户按键（GPIO0）一键静音；
- 默认本机呼号 BI9CPS。

FMO 伴侣提供的一切数据均来源于 FMO 主机接口，本设备不替代 FMO 主机本身。

---

## 功能特性

### UI 显示

- 当前通联呼号大字体显示；
- RX 状态提示；
- 当前站点显示；
- 上次通联呼号显示；
- QSO 数量显示与同步；
- WiFi 信号状态图标；
- 电池状态图标；
- 顶部日期时间显示；
- 底部状态栏；
- 待机时钟页面、省电时钟页面；
- 设置页面（呼号 / 音量 / 背光 / WiFi / 电池 / 站点等）。

### 网络功能

- WiFi STA 连接、断线重连、RSSI 周期刷新、WiFi 扫描；
- FMO Host 配置与 WebSocket URL 自动生成；
- WireGuard VPN（默认关闭，可选开启）。

### WebSocket 功能

- Audio WebSocket：接收 PCM 音频流；
- Event WebSocket：接收通联事件；
- Station WebSocket：获取当前站点、站点列表、QSO 列表；
- QSO 手动同步与增量检查。

### 音频功能

- 开机默认静音，用户手动开启音频；
- 音量设置；
- 音频缓冲与自适应缓冲；
- 蓝牙音频输出预留（默认关闭）。

### 电源与省电

- 电池 ADC 采样、百分比估算、低电量提醒；
- 低电量自动进入省电模式、手动省电模式；
- 背光亮度设置。

### 本适配版新增

- **用户按键（GPIO0）一键静音 / 恢复音量**，屏幕同步提示；
- 默认本机呼号 **BI9CPS**（可在设置页修改）。

---

## 硬件说明（立创实战派 ESP32-S3）

| 项目 | 参数 |
| --- | --- |
| 主控模组 | ESP32-S3-WROOM-1-N16R8（双核 240MHz） |
| Flash | 16 MB |
| PSRAM | 8 MB（Octal） |
| 显示屏 | 2.0 寸 IPS，ST7789，320×240，SPI 接口 |
| 触摸屏 | FT6336 电容触摸，I2C 接口（0x38） |
| 姿态传感器 | QMI8658（三轴加速度 + 三轴陀螺仪，I2C 0x6A） |
| 麦克风 | ZTS6216 双路模拟麦克风 |
| 串口 | CH340K（USB 转串口，最大 2Mbps） |
| 按键 | 一个复位按键、一个用户自定义按键 |
| 接口 | Type-C（供电/下载/调试/USB）、摄像头接口、Micro SD、GH1.25 扩展接口 |
| 官方资料 | https://wiki.lckfb.com/zh-hans/szpi-esp32s3/ |

---

## GPIO 引脚分配

本项目实际使用的引脚（与立创实战派官方定义一致）：

| 功能 | GPIO | 说明 |
| --- | --- | --- |
| LCD 背光 BL | IO42 | 高电平点亮 |
| LCD SPI SCK | IO41 | 80MHz，SPI mode 2 |
| LCD SPI MOSI | IO40 | — |
| LCD DC | IO39 | 数据/命令选择 |
| LCD CS | PCA9557 P0 | I2C 扩展器控制，低电平有效（**时序见故障排除**） |
| LCD RST | 悬空 | 使用软件复位 |
| 触摸 SDA | IO1 | FT6336，I2C 0x38，100kHz |
| 触摸 SCL | IO2 | — |
| I2C 扩展器 | IO1/IO2 | PCA9557，I2C 0x19 |
| 用户按键 | IO0 | BOOT 键，按下为低电平（本适配版用作静音键） |
| 音频功放使能 | IO46 | 见 board_config.h |

> 注：GPIO0 为 Strapping 引脚（上电复位时决定启动模式），运行期间作为普通输入按键使用是安全的；复位/下载模式由 BOOT+复位键组合进入，不影响按键静音功能。

---

## 本适配版相对原版的改动

原版 FMO 伴侣固件基于通用 ESP32（XPT2046 电阻触摸、ESP32 内置 DAC、LCD CS 直连 GPIO）编写。适配到立创实战派 ESP32-S3 后，关键改动如下：

| 模块 | 原版 | 本适配版 |
| --- | --- | --- |
| 显示驱动 | st7789 自定义驱动 | `components/lv_porting/lcd_esp_lcd.c`（esp_lcd 官方路径，SPI3、80MHz、mode 2、16bpp） |
| LCD 片选 | GPIO 直连常低 | PCA9557（I2C 0x19）控制，**复位时 CS 高 → 拉低 → 初始化** 官方时序 |
| 颜色字节序 | LV_COLOR_16_SWAP=1 | **LV_COLOR_16_SWAP=0**（修复彩色雪花花屏） |
| 显示方向 | 软件旋转 | MADCTL 0x60 + LVGL 320×240 无旋转注册（保留 rotate_180 兼容） |
| 触摸芯片 | XPT2046（SPI） | **FT6336（I2C 0x38）**，SWAPXY=y / INVERT_X=n / INVERT_Y=y |
| I2C 驱动 | — | `i2c_manager.c` IDF 5.x 兼容补丁（I2C_NUM_0/1 宏定义、portTICK_PERIOD_MS） |
| 用户按键 | 无 | **GPIO0 按下切换静音**（`main.c` user_btn_task） |
| 默认呼号 | — | **BI9CPS**（`app_config.h` APP_DEFAULT_OWNER_CALLSIGN） |
| 设置页呼号 | 占位符 | 输入框默认填入当前/默认呼号 |

---

## 刷写环境搭建（详细步骤）

### 方式一：Windows 命令行 + ESP-IDF 安装器（推荐，本适配版开发环境）

1. **安装 Python**（≥3.8，本适配版使用 3.12）：从 https://www.python.org/downloads/ 下载安装，勾选 *Add Python to PATH*。

2. **安装 ESP-IDF 5.5.1**：
   - 方式 A（在线安装器）：从乐鑫官网下载 [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/)，选择版本 **5.5.1** 安装（含工具链、CMake、Ninja、Python 虚拟环境）。
   - 方式 B（手动 git clone + 脚本）：
     ```powershell
     git clone --recursive -b v5.5.1 https://github.com/espressif/esp-idf.git F:\esp-idf
     cd F:\esp-idf
     .\install.ps1 esp32s3
     ```

3. **打开 ESP-IDF 环境**：安装完成后，使用开始菜单中的 *ESP-IDF 5.5 PowerShell*（或手动执行）：
   ```powershell
   . $env:IDF_PATH\export.ps1
   ```

4. **安装项目依赖组件**：本项目使用 ESP Component Manager，构建时自动从 registry 拉取 `lvgl`、`trombik__esp_wireguard` 等组件，无需手动安装。若网络受限，可预先执行：
   ```powershell
   idf.py update-dependencies
   ```

5. **确认串口驱动**：立创实战派板载 CH340K，Windows 通常自动安装驱动。插入开发板后在设备管理器确认串口号（本适配版为 **COM5**）。

### 方式二：VSCode + ESP-IDF 插件

1. 安装 VSCode，安装扩展 **Espressif IDF**；
2. 按插件引导安装 ESP-IDF 5.5；
3. 打开工程目录，右下角选择目标芯片 `esp32s3`；
4. 使用插件提供的 *Build / Flash / Monitor* 按钮。

---

## 编译固件

```powershell
# 1. 进入工程目录（建议使用纯 ASCII 路径，避免中文路径编译问题）
cd F:\esp32fmo_real

# 2. 设置目标芯片（首次必须）
idf.py set-target esp32s3

# 3.（可选）配置
idf.py menuconfig

# 4. 编译
idf.py build
```

编译产物：

| 文件 | 路径 |
| --- | --- |
| 应用固件 | `build/esp32-fmo-companion-WireGuard.bin` |
| Bootloader | `build/bootloader/bootloader.bin` |
| 分区表 | `build/partition_table/partition-table.bin` |

> 编译错误日志位于 `build/log/idf_py_stdout_output_*.txt`。

### 分区表

`partitions.csv`（本适配版）：

| 分区 | 类型 | 偏移 | 大小 |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24KB |
| phy_init | data/phy | 0xF000 | 4KB |
| factory | app/factory | 0x10000 | 3MB |
| spiffs | data/spiffs | 0x310000 | 960KB |

---

## 烧录固件

### 方式一：idf.py（推荐）

```powershell
idf.py -p COM5 flash
# 或指定波特率
idf.py -p COM5 -b 230400 flash
```

### 方式二：esptool 手动烧录

本适配版为 **16MB Flash、DIO 模式、80MHz**：

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 230400 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x10000 build\esp32-fmo-companion-WireGuard.bin
```

### 擦除 NVS（可选）

首次刷入或需要恢复出厂（清空 WiFi/呼号等配置）时：

```powershell
python -m esptool --chip esp32s3 -p COM5 erase_region 0x9000 0x6000
```

### 查看串口日志

```powershell
idf.py -p COM5 monitor
```

启动正常的关键日志：

```
main: app start
main: NVS initialized
main: I2C bus initialized, sda=1, scl=2, freq=100000
main: user button (GPIO0) mute feature enabled
```

---

## 首次使用与配置

启动后在 **设置页**（屏幕右下角进入）完成以下配置：

1. **WiFi**：SSID 与密码（首次烧录后 NVS 为空，需重新配置）；
2. **FMO Host**：FMO 主机 IP，如 `192.168.3.165`；程序自动生成 WebSocket 地址：
   - `ws://<host>/audio`（音频）
   - `ws://<host>/events`（事件）
   - `ws://<host>/ws`（站点 / QSO）
3. **本机呼号**：默认 **BI9CPS**，可直接保存生效（用于待机时钟页显示与网关上报）；
4. **音量 / 背光**：默认 60% / 30%；
5. **QSO 同步**：首次无缓存时手动同步，之后自动增量检查；
6. **WireGuard**：默认关闭（`endpoint :51820`、本地 `10.0.0.5`），需要时在 `app_config.h` / 菜单配置开启。

主要配置项见 `main/app_config.h`：

```c
#define APP_VERSION_TEXT "v1.2.1"
#define APP_DEFAULT_OWNER_CALLSIGN "BI9CPS"   // 默认本机呼号
#define DEFAULT_WIFI_SSID       "YOUR_WIFI_SSID"
#define DEFAULT_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define DEFAULT_FMO_HOST        "192.168.3.165"
#define DEFAULT_AUDIO_VOLUME       60
#define DEFAULT_BACKLIGHT_PERCENT  30
```

配置保存在 NVS：`namespace=appcfg`、`key=settings`。

---

## 用户按键静音功能

本适配版将开发板 **用户自定义按键（GPIO0 / BOOT 键）** 设为本地静音控制：

- **短按一次** → 静音（关闭本地功放、清空音频缓冲），屏幕状态栏提示「已静音」；
- **再按一次** → 恢复音量，提示「已恢复音量」；
- 与 FMO 服务端的静音互不影响（仅控制本机喇叭）。

实现位置：`main/main.c`（`user_btn_isr` / `user_btn_task` / `user_btn_init`），核心调用 `local_audio_mute(bool)`（`main/audio_ws.c`）。

---

## 故障排除（常见问题）

以下问题均在本适配过程中实际遇到并解决，按现象归类。

### 1. 屏幕黑屏 / 白屏（背光亮，无内容）

**排查顺序**：

1. **LCD 片选（CS）时序 —— 本适配版黑屏的首要根因**
   原版固件 `board_lcd_cs_init` 将 CS 提前拉低并保持常低，导致 ST7789 的 SWRESET（软件复位）被屏幕真实执行、初始化时序异常 → 黑屏。
   **正确时序**（已内置）：复位时 CS 保持高 → 拉低 CS → 发送初始化命令。
   验证方法：确认 `components/lv_porting/lcd_esp_lcd.c` 通过 PCA9557（I2C 0x19、输出寄存器 0x01、bit0=P0）实现读改写复刻官方时序；切勿把 CS 直接拉低常低。

2. **SPI 参数**：SPI3 主机、80MHz、mode 2、16bpp、RGB 序（与官方一致，勿随意改动）。

3. **FPC 排线**：断电后重新插拔屏幕 FPC 排线并重新上电。

4. **硬件验证（决定性测试）**：刷入立创官方出厂测试固件（fw.gtai-tech.com 在线烧录站）验证——出厂固件能正常显示即说明硬件正常，问题在软件适配层。

### 2. 花屏 / 彩色雪花 / 乱码

- **颜色字节序错误**：RGB565 高低字节反了。将 `components/lvgl/lv_conf.h` 的 `LV_COLOR_16_SWAP` 设为 **0**（本适配版已修正）。字节序错误典型特征是：全屏彩色噪点雪花，仅部分图标清晰。
- **MADCTL 方向错误**：ST7789 RAM 为 240×320（列主序），模组为 320×240 横屏，需 MADCTL 0x60（swap_xy + mirror）完成方向映射（见下）。

### 3. 显示方向不对 / 旋转残留

- 曾尝试 LVGL 注册 240×320 + 软件旋转（ROT_90/270），存在右侧 80px 残留无法覆盖；
- **最终方案（本适配版）**：LVGL 注册 **320×240**、`sw_rotate=0`（不做软件旋转）、面板方向完全交由 **MADCTL 0x60** 处理，并保留 `rotate_180` 兼容选项（`lv_port_disp_set_rotate_180`）。
- 若整体上下颠倒：在 `main.c` 调用 `lv_port_disp_set_rotate_180(true)`。

### 4. 触摸完全无响应

1. **确认 I2C 总线**：触摸芯片 FT6336 地址 **0x38**（SDA=IO1、SCL=IO2、100kHz）。可用 I2C 扫描确认 0x38 应答。
2. **i2c_manager IDF 5.x 兼容补丁（关键）**：ESP-IDF 5.x 中 `I2C_NUM_0` 是**枚举常量**（hal/i2c_types.h）而非预处理宏，导致 `lvgl_esp32_drivers` 的 `i2c_manager.c` 中 `#if defined(I2C_NUM_0)` 编译期判断失效、`lvgl_i2c_read` 报 “Invalid port or not configured for I2C Manager: 0”。
   **补丁**（已内置）：在 `i2c_manager.c` 顶部显式 `#define I2C_NUM_0 0` / `#define I2C_NUM_1 1`，并将全部 `portTICK_RATE_MS` 替换为 `portTICK_PERIOD_MS`（IDF 5.x 改名，不替换会编译错误）。
3. **初始化确认**：日志出现 `FT6X36` 初始化成功（Device ID 0x11、Chip ID 0x64，即 FT6336）即正常。若刷屏 “FT6X36: Init first!” 说明 `lv_port_indev.c` 中 `touch_driver_init()` 未启用。

### 5. 触摸方向错误（点击位置与手指不对应）

坐标方向通过 `sdkconfig` 中三个开关组合校准：

| 配置 | 本适配版取值 | 说明 |
| --- | --- | --- |
| CONFIG_LV_FT6X36_SWAPXY | y | X/Y 交换 |
| CONFIG_LV_FT6X36_INVERT_X | n（关闭） | X 镜像 |
| CONFIG_LV_FT6X36_INVERT_Y | y（开启） | Y 镜像 |

> 校准方法：点屏幕四角记录触摸原始坐标，代数推导出需要交换/镜像的轴。本适配版已按实测四角坐标定案（左上 raw(13,41)、右上 raw(204,40)、右下 raw(224,282)），请勿随意改动此组合。

### 6. 编译错误（常见 IDF 5.x API 差异）

- `portTICK_RATE_MS` 未定义 → 替换为 `portTICK_PERIOD_MS`；
- `I2C_NUM_0` 相关判断失效 → 显式宏定义（见上）；
- 中文路径编译失败 → 将工程放到纯 ASCII 路径（如 `F:\esp32fmo_real`）。

### 7. 用户按键静音不生效

- 确认日志出现 `user button (GPIO0) mute feature enabled`；
- 确认按键接线：GPIO0 上拉、下降沿中断（`GPIO_INTR_NEGEDGE`）；
- 防抖为 30ms，快速连按会被合并，请间隔约 0.5 秒操作。

### 8. WiFi 连不上

- 确认 NVS 已配置正确 SSID/密码（可在设置页配置）；
- 默认 SSID 为 `YOUR_WIFI_SSID`，未配置时日志报 `NO_AP_FOUND`；
- 若 NVS 状态异常，可擦除 NVS 后重新配置（见「烧录固件」）。

---

## 软件架构与目录结构

```text
esp32-fmo-companion-WireGuard/
├── CMakeLists.txt            工程构建入口
├── partitions.csv            分区表（nvs/phy/factory/spiffs）
├── sdkconfig.defaults        默认配置（含触摸方向校准参数）
├── main/
│   ├── main.c                应用入口：初始化序、LVGL 主循环、用户按键静音
│   ├── app_config.h          默认配置（呼号/WiFi/FMO Host/音量/背光）
│   ├── board_config.h        板级 GPIO 与硬件参数
│   ├── app_settings.c/h      NVS 配置管理
│   ├── app_ui.c/h            LVGL 主界面、设置页、待机时钟
│   ├── ui_async.c/h          后台任务安全更新 UI
│   ├── wifi_manager.c/h      WiFi 连接、重连、扫描
│   ├── audio_ws.c/h          WebSocket、网络音频、QSO 同步、本地静音
│   ├── audio_output.c/h      音频输出抽象层
│   ├── bt_audio.c/h          蓝牙音频输出预留
│   ├── event_parser.c/h      Event/QSO JSON 解析
│   ├── station_parser.c/h    Station JSON 解析与请求构造
│   ├── battery_monitor.c/h   电池采样与低电处理
│   ├── app_power_save.c/h    省电模式管理
│   ├── wg_vpn.c/h            WireGuard VPN
│   ├── wg_web_config.c/h     WireGuard Web 配置
│   ├── es8311_codec.c/h      音频编解码器
│   ├── qth_lookup.c/h        QTH 查询
│   ├── gpio_ctrl.c/h         通用 GPIO 控制
│   └── fonts/                LVGL 字体
├── components/
│   ├── lv_porting/           显示/输入移植（lcd_esp_lcd.c 为本适配版新增）
│   ├── lvgl/                 LVGL 图形库（lv_conf.h：LV_COLOR_16_SWAP=0）
│   ├── lvgl_esp32_drivers/   LVGL 驱动（i2c_manager.c 已打 IDF5.x 补丁）
│   └── trombik__esp_wireguard/  WireGuard 组件
└── managed_components/       构建时自动拉取（不入库）
```

启动流程（`main.c app_main`）：

1. 初始化 NVS → 2. 加载应用配置 → 3. 初始化省电管理 → 4. 初始化 LVGL → 5. 初始化显示（esp_lcd + PCA9557 CS 时序）→ 6. 初始化触摸（FT6336）→ 7. 创建 LVGL tick 定时器 → 8. 创建 UI → 9. 初始化音频输出 → 10. 启动 WiFi → 11. WiFi 获取 IP 后启动 WebSocket → 12. 启动电池监测 → 12a. 初始化用户按键静音 → 13. 进入 LVGL 主循环。

---

## 上游功能说明

### 音频格式

服务端音频：raw 16-bit PCM little-endian、mono、8000Hz；输出 32000Hz（4 倍升采样）。开机默认静音，用户点击后开启 Audio WebSocket，播放任务等待缓冲达到阈值后打开功放。

### QSO 同步

- 无缓存：首次启动提示手动同步；
- 有缓存：周期性请求最新一条（page=0、pageSize=1），latestLogId 变化则增量扫描；
- 手动同步：从第 0 页开始，默认最多扫描 20×500=10000 条。

### 省电模式

低电量（≤10% 或 ≤3450mV）或手动触发进入：停止 WebSocket → 停止 WiFi → 省电时钟页 → 背光降至 10%；恢复条件 ≥25% 或 ≥3650mV。

### 蓝牙音频

预留 A2DP Source 输出（`APP_CONFIG_BT_AUDIO_ENABLE`，默认 0）。注意：ESP32-S3 内存有限，启用蓝牙音频会增加内存压力。

---

## 版权与致谢

本项目是 [esp32-fmo-companion](https://github.com/zhaozhengde/esp32-fmo-companion)（原作者 **BI8SIG**）的派生适配版本，代码版权归属原作者，遵循 MIT License。

- **FMO 系统**：感谢 FMO 作者 **BG5ESN** 开发了 FMO 这一开放、独立、去中心化的互联网模拟通联系统；
- **FMO 伴侣固件**：感谢原作者 **BI8SIG**（[zhaozhengde/esp32-fmo-companion](https://github.com/zhaozhengde/esp32-fmo-companion)）开发了 FMO 伴侣固件；
- **开发板**：感谢立创开源硬件平台（LCKFB）提供的立创实战派 ESP32-S3 开发板与开源资料（[wiki.lckfb.com](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)）；
- **测试支持**：感谢 **BI8SPD** 及各位台友在项目开发与测试过程中提供的支持与反馈。

**本项目适配改动**（2026-10-12，BI9CPS）：显示驱动移植（esp_lcd + PCA9557 CS 时序）、颜色字节序、显示方向、FT6336 触摸适配与坐标校准、i2c_manager IDF 5.x 兼容补丁、用户按键静音、默认呼号 BI9CPS。

---

## 许可证

本项目采用 [MIT License](LICENSE) 开源，版权声明（Copyright (c) 2026 BI8SIG）与许可证文本保留原样。你可以自由使用、修改和分发本项目代码，但需保留原作者版权声明和许可证文本。

---

*本 README 由 BI9CPS 于 2026-10-12 整理。*
