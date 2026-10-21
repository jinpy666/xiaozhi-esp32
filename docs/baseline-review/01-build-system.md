# 基线审查 ①：构建脚本与配置字段说明

> 审查基线：上游 `78/xiaozhi-esp32` 最新提交 `aab98ab`（2026-10-19，本地 `main` 与其完全一致，零定制）。
> 本文只描述现状，不含设计；所有结论附源码位置，标注"待验证"的部分需实际构建确认。

## 1. 核心结论（对原方案假设的修正）

1. **仓库不存在顶层 `config.json`**。原方案 3.1 节把 `config.json` 描述为"现有板级/构建配置入口（单个文件）"——实际是**每个板卡目录一份 `config.json`**，共 157 份，包含 191 个 build（固件变体）条目，由 `scripts/build.py` 运行时递归收集。
2. `scripts/build.py`（1974 行）不是简单的"选择配置并调用构建"，而是一套完整的**变体收集 → 校验 → Kconfig 解析 → sdkconfig 合成 → 构建验证**系统。
3. 板级选择的真实链路是四层映射，新增板卡四层缺一不可：

```
main/boards/<厂商>/<板>/config.json   （target + builds）
        │  scripts/build.py: _collect_variants() / _resolve_board_config()
        ▼
main/Kconfig.projbuild: choice BOARD_TYPE（171 个 config BOARD_TYPE_*，均 depends on IDF_TARGET_*）
        │
        ▼
main/CMakeLists.txt:94-900   if/elseif(CONFIG_BOARD_TYPE_*) → set(BOARD_DIR "<目录>")
        │  file(GLOB BOARD_SOURCES boards/${BOARD_DIR}/*.cc *.c)   (main/CMakeLists.txt:936-940)
        ▼
idf.py -DIDF_TARGET=<t> -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;build/xiaozhi-build.sdkconfig.defaults" reconfigure && build
```

## 2. `main/boards/**/config.json` 字段说明

### 2.1 顶层字段（157 份文件实测统计）

| 字段 | 出现次数 | 含义 | 校验位置 |
| --- | --- | --- | --- |
| `type` | 157 | OTA 上报的 board type，必须匹配 `^[a-z0-9.-]+$` | `scripts/build.py:108,123-125` |
| `target` | 157 | 芯片目标，必填非空 | `scripts/build.py:874-876` |
| `builds` | 157 | 固件变体（build 条目）数组 | — |
| `manufacturer` | 131 | 厂商名；必须与 `main/boards/<manufacturer>/` 子目录一致 | `scripts/build.py:888-908` |
| `preview` | 4（仅 esp32s31 板卡） | 布尔；传 `idf.py --preview` | `scripts/build.py:72-79,1546-1548` |

### 2.2 build 级字段（191 个条目实测统计）

| 字段 | 出现次数 | 含义 |
| --- | --- | --- |
| `name` | 191 | OTA 上报的 board name，全仓唯一 | 
| `sdkconfig_append` | 185 | 字符串数组，逐条 `CONFIG_XXX=y/n` 追加项 |
| `build_options` | 12 | 语义化选项默认值（如 `camera_hmirror`） | 
| `legacy_names` | 16 | 历史名称；**仓库内无任何脚本消费它**（全量 grep 仅命中 config.json 自身），疑为外部/服务端元数据 |
| `idf_version` | 6 | 版本门控表达式，如 `"<6.0"`，过滤 builds | 

### 2.3 target 支持值（7 种，不止常见的 5 种）

| target | 板卡目录数 | build 条目数 |
| --- | --- | --- |
| esp32s3 | 117 | 128 |
| esp32 | 6 | 7 |
| esp32c3 | 10 | 10 |
| esp32c6 | 10 | 10 |
| esp32p4 | 6 | 28 |
| esp32c5 | 4 | 4 |
| esp32s31 | 4 | 4 |
| **合计** | **157** | **191** |

与 `scripts/build.py:28-36` 的 `_WAKE_WORD_TARGETS` 集合一致。

### 2.4 代表性条目

最简单（`main/boards/bread-compact-wifi/config.json`，顶层目录板，无 manufacturer）：

```json
{
    "type": "bread-compact-wifi",
    "target": "esp32s3",
    "builds": [
        { "name": "bread-compact-wifi", "sdkconfig_append": [ "CONFIG_OLED_SSD1306_128X32=y" ] },
        { "name": "bread-compact-wifi-128x64", "sdkconfig_append": [ "CONFIG_OLED_SSD1306_128X64=y" ] }
    ]
}
```

较复杂（`main/boards/m5stack/tab5/config.json`）——在 `sdkconfig_append` 中显式写 `CONFIG_BOARD_TYPE_*`、芯片修订与组件引脚参数，且两个 build 同板：

```json
{
    "manufacturer": "m5stack",
    "type": "m5stack-tab5",
    "target": "esp32p4",
    "builds": [
        { "name": "m5stack-tab5", "sdkconfig_append": [
            "CONFIG_BOARD_TYPE_M5STACK_CORE_TAB5=y", "CONFIG_CAMERA_SC202CS=y",
            "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=13", "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y", ... ] },
        { "name": "m5stack-tab5-p4x", "sdkconfig_append": [ ... ] }
    ]
}
```

含语义默认值与 legacy_names（`main/boards/lilygo/t-cameraplus-s3/config.json`）：

```json
{ "manufacturer": "lilygo", "type": "lilygo-t-cameraplus-s3", "target": "esp32s3",
  "builds": [
    { "name": "lilygo-t-cameraplus-s3", "build_options": { "camera_hmirror": true, "camera_vflip": true },
      "sdkconfig_append": ["CONFIG_SPIRAM_MODE_QUAD=y", "CONFIG_CAMERA_OV2640=y", ...] },
    { "name": "lilygo-t-cameraplus-s3-v1.2", "legacy_names": ["lilygo-t-cameraplus-s3_v1_2"], ... } ] }
```

## 3. `scripts/build.py`（1974 行）

### 3.1 命令行参数（argparse 定义 `build.py:1747-1804`）

| 参数 | 说明 |
| --- | --- |
| `board`（位置，可选） | 板卡目录路径；支持 `all` |
| `-c/--config` | config 文件名，默认 `config.json`（`build.py:1752`） |
| `--list-boards` / `--list-languages` / `--list-wake-words` / `--json` | 列举类子命令，互斥 |
| `--name` | 按 build `name` 过滤变体 |
| `--language` | locale，默认 `en-US`（`build.py:23`） |
| `--wake-word` | 唤醒词模型名 / `nihaoxiaozhi` / `hiesp` / `disabled` |
| `--build-options-json` | 语义化 build options 的 JSON 输入 |
| `--zip` | 构建 `merged-binary.bin` 后打包到 `releases/` |
| `--select-changed` | 从 stdin 读变更文件路径列表，输出受影响变体 JSON（CI 用） |

注意：**没有 release 相关参数**；发布矩阵由独立脚本 `scripts/release.py`（import build，经 `gh api` 操作 GitHub releases）负责。

### 3.2 `build_board()` 主流程（`build.py:1512-1694`）

1. 读 `main/boards/<board>/config.json`（`build.py:1534-1543`），取 `target`/`type`/`preview`/`manufacturer`。
2. `_get_builds_for_idf()` 按 `idf_version` 门控过滤 builds（`build.py:804-813,1551`），再按 `--name` 过滤。
3. `_resolve_board_config()`（`build.py:1208-1271`）确定 `CONFIG_BOARD_TYPE_X=y`：优先 `sdkconfig_append` 显式值（必须与 CMake 的 BOARD_DIR 分支匹配，`build.py:1229-1235`），否则由 variant 名推导（`build.py:1246-1254`），再按 Kconfig 的 `depends on IDF_TARGET_*` 过滤（`build.py:1177-1205`）；多候选告警取第一个。
4. 组装 sdkconfig 追加链：board type 选项 → build 的 `sdkconfig_append` → 语言 `CONFIG_LANGUAGE_ZH_CN=y` 等（`build.py:217-221`）→ 唤醒词选项（`build.py:328-377`）→ 语义 build_options 展开（`build.py:662-736`）。
5. `_merge_sdkconfig_options()`（`build.py:1317-1329`，同 key 后者覆盖）→ `_apply_auto_selects()`（`build.py:1295-1314`）：如 `USE_ESP_BLUFI...=y` 时手工补 BT 依赖（`_AUTO_SELECT_RULES`，`build.py:1277-1285`，注释明言"模拟 menuconfig 的 select"）；`CONFIG_SPIRAM=n` 时切 LVGL malloc。
6. `_prepare_target()`（`build.py:1403-1417`）：读 `build/CMakeCache.txt` 的 `IDF_TARGET`，不一致则 `idf.py fullclean`。
7. `_configure_build()`（`build.py:1420-1454`）：**不生成 Kconfig 文件、不用 set-target**；旧 `sdkconfig` 备份为 `sdkconfig.old`，追加链写入 `build/xiaozhi-build.sdkconfig.defaults` 片段，然后 `idf.py -DIDF_TARGET=... -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;build/xiaozhi-build.sdkconfig.defaults" -DBOARD_NAME=<name> reconfigure`（`build.py:1447-1453`）。
8. `_validate_configured_symbols/_validate_configured_options`（`build.py:1457-1509`）回读 sdkconfig 验证所有选项被 Kconfig 接受 → `idf.py build` → `idf.py merge-bin` → `--zip` 时打包。

### 3.3 release 打包与命名

- `zip_bin()`（`build.py:86-97`）：`build/merged-binary.bin` → `releases/v{version}_{final_name}.zip`（zip 内文件名仍为 `merged-binary.bin`）。
- 版本号：解析根 `CMakeLists.txt:12` 的 `set(PROJECT_VER "2.5.3")`（`build.py:50-56`）。
- 固件名：`_get_release_full_name()`（`build.py:148-155`）= `manufacturer-` 前缀（不重复时）+ build `name`；`-p4x` 后缀规范化为芯片段（`build.py:139-145`）。

## 4. `main/Kconfig.projbuild`（1206 行）

### 4.1 结构

整体在 `menu "Xiaozhi Assistant"` 内（第 1 行 ~ 第 1206 行）。**板卡选择是单一 choice**：`choice BOARD_TYPE`（`main/Kconfig.projbuild:124-650`），内含 **171 个** `config BOARD_TYPE_*`，每个 `depends on IDF_TARGET_ESPXX`，含 7 个按 target 的 default（`main/Kconfig.projbuild:126-132`）。

### 4.2 选项组一览

| 组（行号） | 代表性选项 |
| --- | --- |
| choice "Flash Assets"（9-26） | `FLASH_NONE_ASSETS` / `FLASH_DEFAULT_ASSETS` / `FLASH_CUSTOM_ASSETS` / `FLASH_EXPRESSION_ASSETS` + `CUSTOM_ASSETS_FILE`（28）——**现成的资源打包编译期裁剪机制** |
| choice "Default Language"（36-122） | `LANGUAGE_ZH_CN` / `LANGUAGE_EN_US` / `LANGUAGE_JA_JP` / `LANGUAGE_DE_DE` / `LANGUAGE_PT_BR` 等约 40 项 |
| choice BOARD_TYPE（124-650） | 171 项，如 `BOARD_TYPE_BREAD_COMPACT_WIFI`、`BOARD_TYPE_M5STACK_CORE_TAB5` |
| ReSpeaker（652-677） | `RESPEAKER_I2S_EXTERNAL_CLOCK_48K` 等 |
| choice XIAOZHI_NETWORK_TYPE（679-695，仅 p4-nano） | `XIAOZHI_NETWORK_WIFI` / `XIAOZHI_NETWORK_ETHERNET`；menu "Ethernet Configuration"（697-742） |
| 板级子 choice | `ESP32_S3_LCD_EV_BOARD_VERSION`(744)、`WK_ESP32S3_DEV_DISPLAY_TYPE`(755)、`DISPLAY_CHD_ESP32S3_BOX`(839) 等 |
| choice DISPLAY_OLED_TYPE（770-782） | `OLED_SSD1306_128X32` / `OLED_SSD1306_128X64` / `OLED_SH1106_128X64` |
| choice DISPLAY_LCD_TYPE（785-825） | `LCD_ST7789_240X320`、`LCD_ST7735_128X160`、`LCD_ST7796_320X480`、`LCD_ILI9341_240X320`、`LCD_GC9A01_240X240`、`LCD_CUSTOM` |
| choice DISPLAY_STYLE（907-924） | `USE_DEFAULT_MESSAGE_STYLE` / `USE_WECHAT_MESSAGE_STYLE` / `USE_EMOTE_MESSAGE_STYLE` |
| `USE_MULTILINE_CHAT_MESSAGE`（935） | 依赖默认显示风格 |
| choice WAKE_WORD_TYPE（946-977） | `WAKE_WORD_DISABLED` / `USE_ESP_WAKE_WORD` / `USE_AFE_WAKE_WORD` / `USE_CUSTOM_WAKE_WORD`；配套 `CUSTOM_WAKE_WORD*`(979-999)、`SEND_WAKE_WORD_DATA`(1001) |
| 音频处理 | `USE_AUDIO_PROCESSOR`(1018)、`USE_DEVICE_AEC`(1027)、`USE_SERVER_AEC`(1041)、`USE_AUDIO_DEBUGGER`(1048) |
| menu "WiFi Configuration Method"（1054-1070） | `USE_HOTSPOT_WIFI_PROVISIONING`（默认 y）/ `USE_ESP_BLUFI_WIFI_PROVISIONING`（select BT_ENABLED/BT_BLE_BLUFI_ENABLE；PSA Crypto + ffdhe3072） |
| menu "Camera Configuration"（1085-1184） | `XIAOZHI_CAMERA_HMIRROR`、`XIAOZHI_CAMERA_VFLIP` 等 |
| menu TAIJIPAI_S3_CONFIG（1186-1204） | `TAIJIPAI_I2S_TYPE_STD/PDM` 等 |

要点：显示型号 choice 的可用性由 `depends on BOARD_TYPE_*` 决定；**提示音（声音资源）没有任何策略开关**——声音总是通过 `EMBED_FILES` 嵌入（见 §5）。

## 5. `main/CMakeLists.txt`（1507 行）

- **板卡源文件选择**：94-900 行巨型 `if/elseif(CONFIG_BOARD_TYPE_*)` 链先 `set(BOARD_DIR ...)`，并在分支里设置 `BUILTIN_TEXT_FONT/BUILTIN_ICON_FONT/DEFAULT_EMOJI_COLLECTION/EMOTE_RESOLUTION`（默认值 87-90）；随后 `file(GLOB BOARD_SOURCES boards/${BOARD_DIR}/*.cc *.c)`（936-940）。
- **CMake 也读 config.json**：`BOARD_CONFIG_FILE = boards/${BOARD_DIR}/config.json`（906-907），正则提取 `"type"`/`"manufacturer"`（914-922），作为编译定义 `BOARD_TYPE`/`BOARD_NAME`/`BOARD_MANUFACTURER` 注入（1162-1184）；`BOARD_NAME` 由 build.py 经 `-DBOARD_NAME` 传入（`build.py:1450`）。
- **语言与提示音资源**：`CONFIG_LANGUAGE_* → LANG_DIR` 映射（957-1037）；`lang_config.h` 由 `python scripts/gen_lang.py` 生成（1205-1215，目标 `lang_header ALL`）；当前语言的 `.ogg` 缺失时回退 `en-US`（1044-1065）；`main/assets/common/*.ogg` 全语言共用（1067）；**全部声音经 `EMBED_FILES` 嵌入组件**（1126-1127）——即提示音裁剪必须动这条链，不是删一个 ogg 文件。
- **默认 assets.bin**：`scripts/build_default_assets.py` 按 sdkconfig+字体+emoji 生成（1316-1384）；custom/expression 资源处理 1387-1507。
- **字体**：字体名正则拆出 `TEXT_FONT_SIZE/TEXT_FONT_BPP` 编译定义（1172-1183）；字体 bin 来自 `xiaozhi-fonts` 组件（1229-1232）；无 PSRAM 时给 LVGL 注入内存池头（1189-1201）。
- **条件编译实例**（芯片/功能差异如何落源文件）：
  1. `if(CONFIG_XIAOZHI_NETWORK_ETHERNET)` → `boards/common/ethernet_board.cc`（67-69）
  2. 音频引擎按芯片族二选一：S3/P4/S31 → `afe_audio_engine.cc`+`custom_wake_word.cc`，否则 `lite_audio_engine.cc`+`esp_wake_word.cc`（943-950）
  3. `if(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)` → `boards/common/blufi.cpp`（953-955）
  4. esp32 目标剔除 box codec、ml307/nt26 等（1070-1081）；S3/P4 加 `esp_video.cc`+`rndis_board.cc`（1084-1088）；S3 另加 `esp32_camera.cc`（1096-1098）
  5. 按板追加组件依赖（1100-1125），如 p4 EV 板追加 `esp_driver_sdmmc`。

## 6. `scripts/tests/` 宿主测试（10 个文件）

| 文件 | 测什么 |
| --- | --- |
| `test_build.py` | build.py 单测（最大）：版本解析、变体唯一性、type/name 校验、board 选择与 Kconfig menu↔CMake 一致性、语言/唤醒词映射、target 配置/fullclean、sdkconfig 备份替换等 |
| `test_ci_selection.py` | `--select-changed` 变体选择、`scripts/ci/representative-variants.json`（18 个代表变体） |
| `test_release.py` | release.py 的 zip/hash/manifest 逻辑 |
| `test_build_default_assets.py` | 资源打包单测 |
| `test_assets.py` | Assets 实现跑在宿主 + 渲染替身 |
| `test_mcp_server.py` | 宿主编译并测试 MCP dispatcher（C++23，需 libcjson/libmbedtls） |
| `test_ogg_demuxer.py` | ogg 解封装边界 |
| `test_esp_vocat_i2c.py` | I2C 读函数在失败替身下的行为 |
| `test_startup_wake_word_hint.py` | 启动聊天提示语逻辑 |
| `test_spiffs_assets.py` | spiffs 资源生成编码（需 numpy/Pillow） |

## 7. 产品配置层（方案 3.2 节）的天然接入点（现状观察，非设计）

1. **sdkconfig 合并汇聚点**：`build_board()` 中 `_merge_sdkconfig_options(sdkconfig_append, user_options)`（`build.py:1648`）之前——board preset、语言、唤醒词、build_options 全部在此汇聚，产品层在同一位置追加一层合并即可。
2. **现成的"外部 JSON → 校验 → sdkconfig"管线**：`--build-options-json` + `_build_option_definitions()`(`build.py:495`) / `_normalize_build_options()`(631) / `_build_options_sdkconfig()`(662)——与"产品 profile JSON"的形态最接近的既有范式。
3. **builds 过滤点**：`_get_builds_for_idf()`（`build.py:804`）之后，可按产品过滤/改写 builds。
4. **声明式依赖注入先例**：`_AUTO_SELECT_RULES`（`build.py:1277-1285`）。
5. **CMake 注入编译定义先例**：`BOARD_TYPE`/`BOARD_NAME`/`BOARD_MANUFACTURER` 由 config.json/CMake 参数注入（`main/CMakeLists.txt:1162-1184`），产品标识（SKU 等）可用同一机制注入。

## 8. 待验证项（仅靠读码无法确认）

1. `legacy_names` 的消费方：仓库内无脚本引用，是否被 OTA 服务端或外部工具使用无法从代码确认。
2. build.py 用文本正则解析 Kconfig/CMake（`_find_board_config_candidates`、`_kconfig_choice`、`_symbol_supports_target`）与真实 Kconfig/CMake 求值（尤其 `select` 链）是否完全一致，需 `idf.py reconfigure` 验证。
3. `sdkconfig.defaults` + `build/xiaozhi-build.sdkconfig.defaults` 的最终 sdkconfig（含 `_AUTO_SELECT_RULES` 的正确性、`CONFIG_SPIRAM=n` 的 LVGL 分支）需真实配置产物确认。
4. `--list-wake-words` 依赖 `managed_components/espressif__esp-sr/Kconfig.projbuild`（`build.py:38-40,228-232`），该文件需组件拉取后才存在。
5. `preview: true` + `idf.py --preview` 的实际行为（4 块 esp32s31 板）。
6. 157 份 config.json 未做全量编译验证（CI 矩阵已覆盖 PR 代表变体，见审查 ④）。
