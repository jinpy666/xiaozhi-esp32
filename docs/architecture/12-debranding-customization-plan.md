# 12 去特征化与品牌定制方案（OEM 白标改造指南）

> 目标：基于本仓库做出自有品牌产品——唤醒词、服务端、授权播报、设备名称等不再暴露原项目特征。
> 许可前提：项目为 **MIT**（LICENSE，Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.）——允许修改、闭源商用，**须保留原版权声明**；"小智/XiaoZhi"名称与 logo 属商标范畴，**不得在自有产品中复用**。本方案只做品牌替换与自有服务端切换，不涉及绕过任何第三方授权。
> 基线：`aab98ab`；所有位置已逐一实测。

## 1. 特征盘点结果

### 1.1 进固件产物的特征（必改，共 8 项）

| # | 特征 | 位置 | 暴露面 | 改法 |
| --- | --- | --- | --- | --- |
| 1 | `project(xiaozhi)` → 固件 app descriptor `project_name` | `CMakeLists.txt:13` | `esptool image_info` 可见；`scripts/release.py:138` 硬校验 | 改名并同步 release.py（§3.4） |
| 2 | OTA 默认 URL `https://api.tenclass.net/xiaozhi/ota/` | `main/Kconfig.projbuild:5` | 固件默认值，开机会请求 | 改默认值（§3.1） |
| 3 | 唤醒词模型 `nihaoxiaozhi`（你好小智） | esp-sr 组件模型数据，经构建参数选入 | 运行时语音唤醒 | 四路径（§3.2） |
| 4 | 提示音/激活播报语音（含品牌配音） | `main/assets/common/*.ogg`(5) + `locales/<lang>/*.ogg`(12) | 扬声器输出 | 替换或裁剪（§3.3） |
| 5 | 热点 SSID 前缀 `"Xiaozhi"` | `main/boards/common/wifi_board.cc:57` | 手机可见的配网热点名 | 改字符串（§3.5） |
| 6 | BluFi BLE 广播名 `"Xiaozhi-Blufi"` | `main/boards/common/blufi.cpp:15` | BLE 扫描可见 | 改字符串（§3.5） |
| 7 | 日志品牌串（如 codec 初始化日志） | `main/audio/`、`main/boards/**` 约 15 个文件 | 串口日志（不外发） | 批量替换或保留（§3.6） |
| 8 | `BOARD_TYPE/BOARD_NAME/manufacturer`（OTA 上报、User-Agent、MCP serverInfo） | 各板 `config.json` + 编译宏 | 协议字段 | **零改动**——自有板目录自带自有值（§3.4） |

### 1.2 不进固件产物的特征（建议**不动**，降低上游同步冲突）

- `XIAOZHI_I2S_PORT` 宏（`audio_codec.h:22-24`，展开后不含字符串）、`CONFIG_XIAOZHI_*` Kconfig 符号（以太网/摄像头等，只存在于 sdkconfig）。
- Kconfig 菜单标题 `menu "Xiaozhi Assistant"`（`Kconfig.projbuild:1`，仅 menuconfig 可见）。
- `build.py` 内部环境变量 `XIAOZHI_BUILD_STAGES` 等 18 处、组件坐标 `78/*`（工具与组件层）。

### 1.3 已中性（实测无需处理）

40 个语言包的 UI 文案（`language.json`）与全部资源文件名**均不含**"小智/Xiaozhi"字样；唤醒词的**显示文本**来自语言包，本身中性，改成品牌词即可。

### 1.4 工具脚本同步点（改项目名时必须同步）

`scripts/release.py:138`（`meta["project"] != "xiaozhi"` 硬校验）、`scripts/versions.py`（2 处）、`scripts/build.py`（18 处，多为工具层标识）。

## 2. 总策略

**优先"新增覆盖"，最小化"修改上游"**：身份字段（#8）通过自有板目录自然解决；真正需要动上游文件的只有 #1/#2/#5/#6 四处字符串 + 唤醒词/提示音的构建参数与资源（资源在自有分支整体替换）。每处改动登记在下表，上游同步时按单审查：

| 改动 | 文件 | 冲突风险 |
| --- | --- | --- |
| 项目改名 | `CMakeLists.txt:13`、`scripts/release.py:138`、`scripts/versions.py` | 低（单行） |
| OTA URL | `main/Kconfig.projbuild:5` | 低（单行） |
| AP/BLE 名称 | `wifi_board.cc:57`、`blufi.cpp:15` | 低（单行） |
| 唤醒词 | 构建参数/sdkconfig_append（不改代码） | 无 |
| 提示音 | `main/assets/**` 资源替换（按文件覆盖） | 低（按文件） |
| 日志串 | ~15 个文件批量替换 | 中——建议放最后、可跳过 |

## 3. 分项实施

### 3.1 服务端 URL

```kconfig
# main/Kconfig.projbuild:3-7
config OTA_URL
    string "OTA URL" default "https://ota.你的域名.com/xiaozhi/ota/"   ← 改默认值
```

- 运行期覆盖链不变：NVS `wifi.ota_url` > 编译默认值；自有服务端接口规格见 [11 章](11-server-protocol-spec.md)（五段响应 + `/activate` 必带 challenge）。
- 注意 URL 末尾斜杠语义：设备按 `<url>/activate` 拼接（ota.cc:510-515）。

### 3.2 唤醒词（四条路径）

| 路径 | 做法 | 优点 | 代价/限制 |
| --- | --- | --- | --- |
| A. 换 esp-sr 现成模型 | `--wake-word hiesp` 等；可选集由 esp-sr 组件决定（`python3 scripts/build.py --list-wake-words`，模型名匹配 `wn9*`，build.py:34） | 零开发 | 仍是别人的词，选择少；**含"你好小智"必须换掉** |
| B. 乐鑫官方定制训练（**正解**） | 通过 Espressif wakeNet 模型定制服务训练品牌词（如"你好小X"），得到自有 `wn9_xxx` 模型接入 | 真唤醒词体验、品牌专属 | 需向乐鑫定制/商用授权；模型接入走 esp-sr 组件与 Kconfig 选择 |
| C. MultiNet 命令词 | `USE_CUSTOM_WAKE_WORD`，把品牌词配置为命令词 | 无需定制模型 | 唤醒效果/功耗与真唤醒词不同，仅 AFE 机型 |
| D. 禁用语音唤醒 | `WAKE_WORD_DISABLED` + 按键唤醒（板级按键已是现成能力） | 最简单 | 无语音唤醒体验 |

配套：唤醒词**显示文本**在语言包 `language.json`（已中性，直接改品牌词）；`SEND_WAKE_WORD_DATA` 若开启会把唤醒词文本+音频上传服务器，见 FAQ Q1。

### 3.3 授权/激活语音播报与提示音

现状清单（全部经 `EMBED_FILES` 无条件打包，播放链见 08 章）：

- 公共 5 个：`exclamation / low_battery / popup / success / vibration`
- 语言包 12 个：`0~9`（激活码数字播报）、`activation`（激活提示）、`err_pin / err_reg`、`upgrade`、`welcome`、`wificonfig`

**路径 A：整包替换配音（推荐首发，零代码）**

1. 保持文件名与键不变（`gen_lang.py` 按 language.json 登记生成 `Lang::Sounds::OGG_*` 符号）。
2. 录制自有品牌配音，转码约束（OggDemuxer 硬性要求，ogg_demuxer.cc:297-359）：**Opus-in-Ogg、单声道、采样率 8/12/16/24/48k**。
3. 数字 `0~9` 可保留通用读音或一并替换；`welcome/activation` 必须换成品牌口播。
4. 各语言按市场需要替换，未替换语言自动回退 en-US（CMakeLists.txt:1044-1065）。

**路径 B：产品级裁剪（不播语音，改屏幕/LED 反馈）**

按 [04 章 §3](04-feature-trimming.md) 三处联动：调用点（激活播报 application.cc:754,760 等 7 处）→ `EMBED_FILES` 过滤（CMakeLists.txt:1126-1127）→ `gen_lang.py` 符号生成。**红线**：只裁反馈不动业务——激活轮询、配网、错误处理全部保留（源码已分离，见 05/06 章）。建议以产品级 Kconfig（如 `PRODUCT_SOUND_POLICY`）承载，不要散改。

### 3.4 项目名与设备身份

```sh
# ① 改项目名（app descriptor 的 project_name）
CMakeLists.txt:13   project(xiaozhi) → project(你的产品名)
# ② 同步工具校验
scripts/release.py:138  "xiaozhi" → 新名
scripts/versions.py     2 处
```

- 设备身份字段（OTA 上报 `board.type/name/manufacturer`、User-Agent、MCP serverInfo）**不需要改任何核心代码**：为自有硬件新增自有板目录（[03 章 §5](03-board-identification.md)），`config.json` 里写自有 `type/manufacturer` 即全自动跟随。
- 遵守仓库规则：**不改既有板卡的任何字段**（OTA 兼容性），只新增。
- 版本号 `PROJECT_VER`（CMakeLists.txt:12）按自有版本策略重排。

### 3.5 热点名与 BluFi 广播名

```cpp
main/boards/common/wifi_board.cc:57   config.ssid_prefix = "Xiaozhi";   → 自有前缀
main/boards/common/blufi.cpp:15       #define BLUFI_DEVICE_NAME "Xiaozhi-Blufi" → 自有名
```

配网 Web 页面已通过 OEM fork 实现品牌化：`components/esp-wifi-connect/`（上游
`78/esp-wifi-connect` v3.3.1 的本地组件，见 `components/README.md`）提供
`/brand.js` 与 `/brand-logo` 端点、页面内嵌槽位与 gzip 下发。品牌参数经
`product/profiles/<tenant>.json` + `scripts/build.py --brand` 注入；整页替换与
接口契约见 [`docs/portal-contract.md`](../portal-contract.md)。

### 3.6 日志串清理（可选项）

`main/audio/` codecs 与部分板级文件约 15 处含 "Xiaozhi" 日志文案（仅串口可见，不外发）。批量 `grep -rl` + 替换即可；因文件多、上游冲突面大，**建议放在最后一次同步基线之后做**，或接受保留。

### 3.7 视觉资产（可选）

开机 Logo/主题/表情在 assets 体系（`FLASH_CUSTOM_ASSETS` 支持外置资源包，[04 章 §2](04-feature-trimming.md)）；OLED/LVGL 初始 UI 文案已中性。品牌视觉随产品 UI 需求另做。

## 4. 残留审计（验收，每次发布前跑）

```sh
# ① 固件产物中搜品牌残留（二进制字符串层）
strings build/merged-binary.bin | grep -iE "xiaozhi|tenclass" | sort -u
# ② app descriptor 校验（项目名/版本）
esptool.py image_info build/merged-binary.bin | grep -iE "project|version"
# ③ 唤醒词模型确认（非 nihaoxiaozhi）
python3 scripts/build.py --list-wake-words
# ④ 提示音抽查：试听替换后的 ogg；确认体积变化符合预期
```

功能冒烟（对应 [../baseline-review/](../baseline-review/) 验收清单）：自有服务端 CheckVersion/激活全流程、新唤醒词真机唤醒、配网（热点名/页面可达）、TTS/提示音播放、OTA 升级。

## 5. 第二阶段：收敛到产品配置层

上述改动落地后，把可变点收敛为 `product/profiles/<sku>.json`，由 build.py 合并注入（扩展点见 [02 章 §7](02-build-pipeline.md)），实现"一套代码、多品牌构建"：

> 已落地：`python3 scripts/build.py <board> --brand <tenant>`。档案字段、暂存物、
> 优先级与配网页三档定制见 [`product/README.md`](../../product/README.md) §8。

| 品牌字段 | 注入方式 |
| --- | --- |
| 产品名/project 名 | 编译宏（参照 `BOARD_NAME` 注入机制，CMakeLists.txt:1162-1184） |
| OTA URL、AP 前缀、BLE 名、声音策略 | `sdkconfig_append` 追加层 + 产品级 Kconfig |
| 唤醒词 | 既有 `--wake-word` 通道 |
| 提示音语言包/资源 | 语言目录选择 + `FLASH_CUSTOM_ASSETS` |

## 6. 实施顺序与工作量估算

| 阶段 | 内容 | 估时 |
| --- | --- | --- |
| P0 | 许可确认（保留 LICENSE）、自有服务端就绪（对照 11 章规格） | 0.5 天 |
| P1 | 四处字符串改点 + 项目名同步（#1/#2/#5/#6） | 0.5 天 |
| P2 | 提示音整包替换（路径 A）+ 唤醒词路径决策与接入 | 1–3 天（含录音转码/定制词周期另计） |
| P3 | 自有板卡与身份字段、构建验证 | 1–2 天 |
| P4 | 残留审计 + 真机冒烟 | 1 天 |
| P5（可选） | 提示音裁剪开关、产品配置层收敛、日志串清理 | 2–4 天 |

注意：唤醒词定制训练（路径 B）的外部周期不可控，应最早启动；P1–P4 可与其并行。

## 7. 附录：可感知特征点详解（提示音 / 播报 / 配网 / 授权码）

> 本节为 §3 的深化，全部机制经源码逐行核实（基线 `aab98ab`）。

### 7.1 提示音（音效包）

现状机制：

- **注册是目录扫描而非清单**：`scripts/gen_lang.py:114-120` 取 `locales/en-US` + 当前语言 + `assets/common` 三处 `.ogg` 文件名并集，按文件名生成 `Lang::Sounds::OGG_<NAME>` 符号——**文件名即接口**，`language.json` 只含 `strings` 文案（无声音清单）。
- 语言回退：当前语言缺失的音回落 en-US（`main/CMakeLists.txt:1044-1065`）；`assets/common` 5 个全语言共用。
- 打包：`EMBED_FILES` 无条件嵌入（1126-1127）；播放格式硬约束：**Opus-in-Ogg、单声道、8/12/16/24/48kHz**（`ogg_demuxer.cc:297-359`）。

自定义层级：

| 层级 | 做法 | 改动面 |
| --- | --- | --- |
| 换配音 | **同名**替换 `.ogg` 文件 | 纯资源，零代码 |
| 加语言 | 新建 `locales/<lang>/`（声音 + language.json） | 资源；Kconfig 语言选项已有 40 个 |
| 裁剪类别 | 三处联动（调用点/EMBED_FILES/gen_lang） | 见 §3.3 路径 B |

注意：`welcome.ogg` 在主流程未见调用（遗留资产，换品牌时可借机清理）。

### 7.2 播报（本地语音 + 屏幕）

现状：所有本地播报走统一原语 `Alert(status, message, emotion, sound)`（`application.cc:765-775`）= 状态栏 + 表情 + 消息气泡 + 音效，共 7 类调用点（错误/激活/配网/SIM/低电量/服务器 alert/通知前提示）。两个特例：激活码逐位播报（§7.4）、配网提示动态拼接（§7.3）。**服务器 TTS 语音不属固件特征**（音色由服务端决定）。

自定义层级：

| 对象 | 载体 | 改动面 |
| --- | --- | --- |
| 屏幕文案（58 条） | `language.json` `strings`——品牌语气载体：`WAKE_WORD_HINT`（"说「%s」就能唤醒我。"）、`HELLO_MY_FRIEND`（"你好，我的朋友！"）、`CONNECT_TO_HOTSPOT`/`ACCESS_VIA_BROWSER`（配网提示模板）等 | 纯资源，逐条改 |
| 表情 | `Alert` 的 `emotion` 字符串 → 显示层图标 | 显示层（可选） |
| 本地语音 | 音效包替换（同 §7.1） | 资源 |
| TTS 音色 | 服务器 | 服务端 |

### 7.3 配网

现状：

- 热点路径：`StartConfigAp()`（组件）→ 延迟 `Alert` 拼接 `CONNECT_TO_HOTSPOT + AP名 + ACCESS_VIA_BROWSER + 网址`（`wifi_board.cc:174-187`）——**语音 `wificonfig.ogg` 固定，屏幕文字动态含真实 AP 名与网址**。
- BluFi 路径：`blufi.init()`，**无语音提示**。
- 品牌名：AP 前缀 / BLE 名已 Kconfig 化（§3.5，`PRODUCT_BRAND_AP_PREFIX/BLE_NAME`）。
- 配网页面（HTML/交互）：已由 `components/esp-wifi-connect/`（OEM fork）品牌化，
  支持换肤、声明式内容定制和整页替换三档，契约见 [`../portal-contract.md`](../portal-contract.md)。
- 配网态扬声器测试：`kDeviceStateAudioTesting` + `EnableAudioTesting()`（`application.cc:805-809,869-899`），测试包走 testing 队列（与解码队列 swap 复用）。

自定义层级：

| 对象 | 做法 | 改动面 |
| --- | --- | --- |
| AP/BLE 名称 | Kconfig 覆盖 | 配置（已就绪） |
| 配网页面内容/交互 | fork `78/esp-wifi-connect` | 组件 |
| 配网语音 | `wificonfig.ogg` 替换 | 资源 |
| 进入条件（60s 超时、主动入口） | `wifi_board.cc` | 代码（不建议动，行为敏感） |

### 7.4 授权码（激活）

现状：

- 码的内容、长度、字符集、有效期**完全由服务器定义**（CheckVersion 响应 `activation.code` 任意字符串）；设备只负责展示与轮询（10 次 `/activate`，202=等用户输入）。
- 设备展示 = 屏幕（状态栏"激活设备" + 服务器下发的 `activation.message` + 码本身）+ 语音：先播 `activation.ogg`，再**遍历码的每个字符、仅 `'0'-'9'` 播对应数字音，非数字字符静默跳过**（`application.cc:741-763`）。
- v2 的 challenge + eFuse HMAC 依赖产测烧写（HMAC_KEY0 + USER_DATA 序列号），OEM 需自建产测流程或先用 v1。

自定义层级：

| 对象 | 做法 | 改动面 |
| --- | --- | --- |
| 码规则 / 绑定关系 / message 文案 | 自有服务端全权（message 是服务器下发的屏幕文案） | 服务端 |
| 展示方式（屏 / 语音 / LED / 二维码） | 改 `ShowActivationCode` 一个函数 | 代码（单点） |
| 关闭激活语音播报 | §3.3 路径 B（业务逻辑不受影响） | 资源 + 开关 |

**OEM 关键提醒**：设备语音只支持数字播报——自有服务端的激活码建议用**纯数字码**（含字母的码用户听不见那几位）；需要字母/二维码展示则在 `ShowActivationCode` 单点扩展。
