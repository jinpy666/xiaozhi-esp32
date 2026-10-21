# 基线审查 ②：板卡、芯片目标与硬件驱动清单

> 审查基线：上游 `78/xiaozhi-esp32` 提交 `aab98ab`。所有数据来自对仓库文件的直接解析，附来源位置。

## 1. 规模统计（三种口径，数值不同，注意区分）

| 口径 | 总数 | 说明 |
| --- | --- | --- |
| `main/boards/` 一级子目录 | **52**（含 `common`，即 51 个厂商/系列目录） | 实测 `ls -d main/boards/*/` |
| 板卡 config.json 所在目录 | **157** | 即"板卡目录数" |
| config.json 内 builds 固件条目 | **191** | 即"可构建变体数" |
| Kconfig `BOARD_TYPE_*` 条目 | **171** | `main/Kconfig.projbuild:124-650` |

三口径不等的原因：同一板卡目录可有多个 build（不同屏/内存/IDF 版本门控），同一目录也可服务多个 Kconfig 条目（硬件版本族）。

**同目录多 build / 多 Kconfig 条目实例**：

- `waveshare/esp32-p4-wifi6-touch-lcd` 一个目录服务 8 个 Kconfig 条目（4B/4.3/5/7B/3.4C/4C/7/8/10.1 等尺寸，`main/CMakeLists.txt:534-583`）。
- `m5stack/corep4/config.json` 内含 2 个 builds（`corep4` 要求 IDF<6.0、`corep4x` 要求 IDF>=6.0）。
- `lilygo/t-cameraplus-s3`：V1.0/V1.1 与 V1.2 共目录（`main/CMakeLists.txt:613-632`）。
- `waveshare/esp32-s3-touch-amoled-1.75` 与 `…-1.75C` 共用目录（`main/CMakeLists.txt:401-410`）。

## 2. 各芯片目标分布

| target | 板卡目录数 | builds 数 | 代表性板卡 |
| --- | --- | --- | --- |
| esp32s3 | 117 | 128 | `alientek/atk-dnesp32s3-box`、`espressif/esp32-s3-box-3`、`m5stack/core-s3`、`bread-compact-wifi-s3cam`、`kevin/sp-v4-dev` |
| esp32 | 6 | 7 | `bread-compact-esp32`、`m5stack/atommatrix-echo-base`、`waveshare/esp32-touch-lcd-3.5` |
| esp32c3 | 10 | 10 | `kevin/c3`、`lckfb/szpi-esp32c3`、`magiclick/c3`、`espressif/esp-hi` |
| esp32c6 | 10 | 10 | 全部为 waveshare 系列（amoled-1.8、lcd-1.69、epaper-1.54 等） |
| esp32p4 | 6 | 28 | `espressif/esp32-p4-function-ev-board`、`m5stack/corep4`、`m5stack/tab5`、`waveshare/esp32-p4-nano`、`lilygo/t-display-p4` |
| esp32c5 | 4 | 4 | waveshare c5 系列 |
| esp32s31 | 4 | 4 | 预览板（config.json 带 `preview: true`） |

**S3 是绝对主力（约 73% 板卡目录 / 67% 变体）**。板卡的 target 定义在各板目录 `config.json` 的 `"target"` 字段（如 `main/boards/m5stack/corep4/config.json:4`），`main/CMakeLists.txt:906` 读取该文件。

## 3. `main/boards/common/` 模块清单（46 个文件）

### 3.1 网络接入层（Board 基类家族）

| 文件 | 职责 | 编译条件 |
| --- | --- | --- |
| `board.h/.cc` | Board 抽象基类（聚合 AudioCodec/Display，持有 UUID） | 总是 |
| `wifi_board.h/.cc` | Wi-Fi 接入板基类（含配网入口） | 总是 |
| `ml307_board.h/.cc` | 移远 ML307 4G Cat.1 模组板基类 | 总是（esp32 目标剔除） |
| `nt26_board.h/.cc` | 移远 NT26 4G 模组板基类 | 总是（esp32 目标剔除） |
| `dual_network_board.h/.cc` | Wi-Fi/4G 双网络切换板基类 | 总是 |
| `ethernet_board.h/.cc` | 以太网板基类 | 仅 `CONFIG_XIAOZHI_NETWORK_ETHERNET`（`main/CMakeLists.txt:67-69`） |
| `rndis_board.h/.cc` | USB RNDIS 网络板 | 仅 S3/P4（`main/CMakeLists.txt:1084-1088`） |
| `blufi.h/.cpp` | BluFi 配网单例 | 仅 `CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING`（`main/CMakeLists.txt:953-955`） |

注意：**不存在 `wifi_configuration_ap.cc`**——AP 配网（热点名、Web 页面、HTTP 接口）在外部托管组件 `78/esp-wifi-connect: ~3.3.1`（`main/idf_component.yml:27`）内实现。

### 3.2 摄像头

| 文件 | 职责 | 编译条件 |
| --- | --- | --- |
| `camera.h` | Camera 抽象接口（拍照/视觉问答） | 总是 |
| `esp32_camera.h/.cc` | 基于 esp32-camera 组件的实现 | 仅 S3（`main/CMakeLists.txt:1096-1098`） |
| `esp_video.h/.cc` | 基于 esp_video（V4L2 风格）的实现 | 仅 S3/P4/S31（`main/CMakeLists.txt:1084-1093`） |

### 3.3 显示/电源/输入/其他

| 文件 | 职责 |
| --- | --- |
| `backlight.h/.cc` | 背光基类 + `PwmBacklight`（LEDC PWM） |
| `adc_battery_monitor.h/.cc` | ADC 分压采样电池电量监测 |
| `axp2101.h/.cc`、`sy6970.h/.cc`、`xpower.h` | AXP2101 / SY6970 / AXP192·202·2101 统一 PMIC 封装 |
| `power_save_timer.h/.cc`、`sleep_timer.h/.cc` | 省电/深睡定时器 |
| `button.h/.cc` | 按键（含 `AdcButton`、`PowerSaveButton`） |
| `knob.h/.cc` | 旋转编码器 |
| `i2c_device.h/.cc` | I2C 设备基类 |
| `system_reset.h/.cc` | 按键组合触发 NVS/出厂复位 |
| `lamp_controller.h` | GPIO 台灯 MCP 工具 |
| `press_to_talk_mcp_tool.h/.cc` | "按住说话" MCP 工具 |

所有 `.cc` 默认参与编译（`main/CMakeLists.txt:49-66`），仅上表标注条件的除外。

## 4. 音频链路组件（`main/audio/`）

### 4.1 Codec 驱动（7 个，`audio/codecs/`）

`audio_codec`（基类，audio/ 根）、`es8311_audio_codec`、`es8374_audio_codec`、`es8388_audio_codec`、`es8389_audio_codec`、`box_audio_codec`（ES8311+ES8388 盒式组合）、`no_audio_codec`（内建 DAC）、`dummy_audio_codec`（空实现）。
源文件注册：`main/CMakeLists.txt:7-13`；**esp32 目标剔除 box/es8388/es8389**（`main/CMakeLists.txt:1070-1081`）。

### 4.2 引擎与唤醒词（按芯片族二选一）

- `engines/afe_audio_engine.cc` —— AFE（FD AEC+VAD+WakeNet），S3/P4/S31 用（`main/CMakeLists.txt:943-946`）
- `engines/lite_audio_engine.cc` —— 轻量引擎，ESP32/C3/C5/C6 用（`main/CMakeLists.txt:948-949`）
- `wake_word.h` 抽象接口 + `wake_words/esp_wake_word.cc`（独立 WakeNet）、`wake_words/custom_wake_word.cc`（MultiNet 命令词）、`wake_words/wake_word_audio_cache.cc`（唤醒前 2 秒 64KB PSRAM 环形缓存）
- Kconfig 4 选 1（`main/Kconfig.projbuild:946-977`）：`WAKE_WORD_DISABLED` / `USE_ESP_WAKE_WORD` / `USE_AFE_WAKE_WORD`（需 PSRAM）/ `USE_CUSTOM_WAKE_WORD`

### 4.3 服务主体

`audio_service.h/.cc`（输入/输出/唤醒/播放统一服务，含 `PlaySound`，`main/audio/audio_service.h:132,147`）；旧的 `AudioProcessor+WakeWord` 组合已被单一 `AudioEngine` 接口取代（`main/audio/README.md` 开头）。

## 5. 显示与 LED

**`main/display/`**：

- `display.h/.cc` —— Display 基类 + `NoDisplay` + Theme
- `lcd_display.h/.cc` —— `LcdDisplay : LvglDisplay`，含 **SpiLcdDisplay / RgbLcdDisplay / MipiLcdDisplay** 三种总线
- `oled_display.h/.cc` —— SSD1306 类小屏
- `lvgl_display/` —— LVGL UI 层（font/image/theme/emoji/动态字形缓存，`gif/`、`jpg/`）
- `emote_display.h/.cc` —— 表情动画播放器（esp-vocat 等）
- `epd_display/` —— 电子纸，仅 maza-ai 板附加编译（`main/CMakeLists.txt:218-231`）
- `lvgl_psram_pool.h` / `lvgl_no_psram_pool.h` —— 按 PSRAM 有无切换内存池（`main/CMakeLists.txt:1189-1201`）

**`main/led/`**：`led.h`（基类 + `NoLed`）、`single_led`、`gpio_led`（esp32 目标剔除）、`circular_strip`（WS2812）。

## 6. 芯片差异的处理机制（三层，无 `idf::esp32s3` 式组件链接）

全仓（除 managed_components）**不存在**按芯片的组件链接条件；芯片差异通过：

1. **CMake 源文件条件（Kconfig 宏）**：音频引擎二选一（`main/CMakeLists.txt:943-950`）。
2. **CMake 源文件剔除/追加**：esp32 剔除 box codec、4G 板类、gpio_led、jpg（1070-1081）；S3/P4 加 `esp_video.cc + rndis_board.cc`，S3 另加 `esp32_camera.cc`（1084-1098）。
3. **按板追加组件依赖**：如 p4 EV 板追加 `esp_driver_sdmmc + sdmmc`（`main/CMakeLists.txt:1120-1125`）。
4. **源码内 `CONFIG_IDF_TARGET_*` 宏**：如 `main/audio/audio_service.cc:20,82,672,700`、`main/system_info.cc:12,39`、`main/display/lcd_display.cc:537`。

## 7. 其他外设现状

- **电源管理**：部分板卡自带 `power_manager.h`（du-chatx、otto-robot 等）+ common 的省电/深睡定时器与 PMIC 驱动。
- **NFC**：仅 zectrix 板板级私有实现（`main/boards/zectrix/zectrix-s3-epaper-4.2/zectrix_nfc.h/.cc`），无 common 层。
- **SD/SDMMC**：p4 EV 板、`nologo/xingzhi-abs-2.0`、`lilygo/t-display-p4`；EV 板靠 CMake 条件追加 SDMMC 组件。
- **触摸屏**：**无 common 抽象层**，约 38 个板卡 `.cc` 直接使用 esp_lcd_touch 组件（CST816/GT911/FT6336 等）。

## 8. 发现的仓库问题（同步/改造前需知）

1. **`main/boards/kevin/sp-v3-dev` 悬空条目**：Kconfig/CMake 引用 `kevin/sp-v3-dev`（`main/CMakeLists.txt:153-154`），目录存在（`config.h` + `kevin-sp-v3_board.cc`）但**无 config.json**，构建/发布流程无法覆盖，疑似遗留。
2. **`espressif/esp-spot` target 疑点**：config.json 声明 `target: esp32c5` 且仅 1 个 build（esp-spot-c5），但 Kconfig 存在 `BOARD_TYPE_ESP_SPOT_S3` 且 `depends on IDF_TARGET_ESP32S3`（`main/Kconfig.projbuild:251-253`），而各 config.json 的 `sdkconfig_append` 中均无 target 覆盖——S3 变体如何选定 target 需构建验证。

## 9. 待验证项

1. 以上 191 个变体未做全量编译（CI 的代表变体机制见审查 ④）；各 target 能否编译通过以 CI/本地构建为准。
2. 触摸控制器型号分布（38 处）仅做了存在性确认，未逐板归类。
3. `esp32c5`/`esp32s31` 属较新目标，工具链与 `preview` 行为需实际构建确认。
