# 基线审查 ③：激活、提示音、配网与设备注册调用关系

> 审查基线：上游 `78/xiaozhi-esp32` 提交 `aab98ab`。所有结论附源码位置；与外部服务器/组件交互的部分以固件侧代码为准，实际响应需联调验证。

## 1. 上电启动流程

```
app_main() → nvs_flash_init() → Application::Initialize() → app.Run()
  main/main.cc:14-28
```

`Initialize()`（`main/application.cc:66-179`）顺序：置 `kDeviceStateStarting` → 显示 SetupUI → 音频服务初始化/启动 → 注册状态机监听 → 注册网络事件回调（含 SIM 卡错误提示音，application.cc:156-167）→ `board.StartNetwork()`（application.cc:175）。

`Run()` 主事件循环（`main/application.cc:181-296`）关键分支：

- `MAIN_EVENT_NETWORK_CONNECTED` → `HandleNetworkConnectedEvent()`（application.cc:204-206, 298-323）
- `MAIN_EVENT_ACTIVATION_DONE` → `HandleActivationDoneEvent()`（application.cc:212-214, 342-376）

**激活链**：`HandleNetworkConnectedEvent()` → 处于 Starting/WifiConfiguring 时置 `kDeviceStateActivating`（application.cc:304）→ `ActivationTask()`（application.cc:378-393）→ `CheckNewVersion()`（application.cc:455）→ `InitializeProtocol()`（application.cc:544）→ `MAIN_EVENT_ACTIVATION_DONE` → `HandleActivationDoneEvent()`：置 `kDeviceStateIdle`、播放 `OGG_SUCCESS`（application.cc:346,373）。

## 2. 设备状态机

12 个状态（`main/device_state.h:4-17`）：`Unknown, Starting, WifiConfiguring, Idle, Connecting, Listening, Speaking, Notifying, Upgrading, Activating, AudioTesting, FatalError`。

关键合法迁移（`main/device_state_machine.cc:35-107`）：

- Starting → WifiConfiguring | Activating
- WifiConfiguring → Activating | AudioTesting
- Activating → Upgrading | Idle | WifiConfiguring
- Idle → Connecting | Listening | Speaking | Notifying | Activating | Upgrading | WifiConfiguring
- FatalError 不可迁出

**架构要点**：`WifiConfiguring`（配网）与 `Activating`（激活）是两个独立状态，**配网与激活在状态机层面已解耦**——这直接支持方案 4.2 节"分离业务状态与反馈通道"的目标。

## 3. OTA / 激活流程（`main/ota.cc`）

### 3.1 服务器地址

`Ota::GetCheckVersionUrl()`（`ota.cc:56-63`）：先读 NVS（命名空间 `"wifi"`，键 `ota_url`），为空则用 `CONFIG_OTA_URL`（`main/Kconfig.projbuild:3-7`，默认 `https://api.tenclass.net/xiaozhi/ota/`）。

> **对自有服务端的意义**：OTA 地址既有编译期默认（CONFIG_OTA_URL）又有 NVS 覆盖通道；协议不变时接入自有服务端的最小改动就是改默认值。

### 3.2 CheckVersion（`ota.cc:87-258`）

请求头（`SetupHttp()`，`ota.cc:65-82`）：

| 头 | 值 |
| --- | --- |
| `Activation-Version` | 有 eFuse 序列号为 "2"，否则 "1" |
| `Device-Id` | `SystemInfo::GetMacAddress()`（冒号分隔小写 MAC，`main/system_info.cc:35-47`） |
| `Client-Id` | `board.GetUuid()`（NVS `"board"` 持久化的随机 UUID v4，`main/boards/common/board.cc:14-43`） |
| `Serial-Number` | 可选 |
| `User-Agent` | `BOARD_NAME "/" app_version`（`main/system_info.cc:53-57`） |
| `Accept-Language` | `Lang::CODE` |

body 为 `board.GetSystemInfoJson()`（version/language/flash_size/mac_address/uuid/chip_info/elf_sha256 等，`board.cc:61-130`），非空则 POST 否则 GET。

响应 JSON 五个 section（`ota.cc:137-254`）：`activation`（message/code/challenge/timeout_ms）、`mqtt`（整对象写入 NVS `"mqtt"`）、`websocket`（写入 NVS `"websocket"`）、`server_time`（settimeofday）、`firmware`（version/url/force，触发升级流程）。

### 3.3 激活接口

`Ota::Activate()`（`ota.cc:504-542`）→ POST `{CheckVersionUrl}/activate`。

- **v1**（无 eFuse 序列号）：payload 为 `"{}"`（`ota.cc:468-470`），设备无密码学凭据。
- **v2**（有 eFuse 序列号，来自 `ESP_EFUSE_USER_DATA`，`ota.cc:29-51`）：用 **eFuse HMAC Key0** 对服务器 challenge 做 HMAC-SHA256（`esp_hmac_calculate(HMAC_KEY0, ...)`，`ota.cc:477`），payload 含 `algorithm/serial_number/challenge/hmac`（`ota.cc:490-494`）。
- 返回 202 = 等待用户确认（超时重试）；200 = 激活成功。

### 3.4 激活码展示

`CheckNewVersion()` → `HasActivationCode()` → `ShowActivationCode(code, message)`（application.cc:522-523, 741-763）：先 `Alert(..., OGG_ACTIVATION)` 提示，再**逐位播放 `OGG_0`~`OGG_9`**（application.cc:746-761），随后循环 10 次 `ota_->Activate()` 轮询（application.cc:527-540）。

> **对方案 4.2 的意义**：激活的业务判断（轮询 activate 接口）与提示反馈（Alert/播报数字）是分开的两段代码；关闭提示音不影响激活逻辑本身。

## 4. 设备身份现状（对照方案第六节的四类）

| 方案中的类别 | 现状实现 | 位置 |
| --- | --- | --- |
| Device ID | MAC 地址 + NVS 持久化 UUID v4 | `system_info.cc:35-47`；`board.cc:14-43` |
| Device Credential（证明设备身份） | v1：**无**（裸 MAC/UUID 上报）；v2：eFuse HMAC_KEY0 烧写 + challenge 应答 | `ota.cc:29-51,467-502` |
| 通信凭据（短期凭据） | **完全由服务端经 OTA 响应下发**（mqtt 对象含 username/password，websocket 对象含 token/url），设备写入 NVS 后被动使用，无本地计算 | `ota.cc:160-199`；`mqtt_protocol.cc:80-86`；`websocket_protocol.cc:97-106` |
| User Binding（用户绑定） | 激活码机制：设备播报数字 → 用户在服务端输入 → `activate` 接口从 202 变 200。绑定关系完全在服务端 | `application.cc:522-540`；`ota.cc:504-542` |
| Feature Entitlement（功能授权） | **不存在**。设备侧无任何功能授权判断 | — |

结论：方案第六节要求的"独立、可撤销、每台设备唯一"凭据体系，v2（eFuse HMAC）已有骨架但依赖产测烧写流程；v1 设备完全没有设备级凭据；功能授权体系需全新实现。

## 5. 配网流程（`main/boards/common/wifi_board.cc`）

### 5.1 触发路径

- `WifiBoard::StartNetwork()`（`wifi_board.cc:52-98`）→ `TryWifiConnect()`（`wifi_board.cc:100-115`）：无已存 SSID → 延时后 `StartWifiConfigMode()`；有 SSID 但 60s 连接超时 → `OnWifiConnectTimeout()`（`wifi_board.cc:162-168`）→ `StartWifiConfigMode()`。
- 运行期主动进入：`EnterWifiConfigMode()`（`wifi_board.cc:195-235`），由板级按键/界面调用；会先 `Application::ResetProtocol()`。

### 5.2 配网方式：**编译期开关已存在**

`StartWifiConfigMode()`（`wifi_board.cc:170-193`）：置 `kDeviceStateWifiConfiguring` 后按 Kconfig 分支（`main/Kconfig.projbuild:1054-1070` 的 menu "WiFi Configuration Method"）：

- `CONFIG_USE_HOTSPOT_WIFI_PROVISIONING`（默认 y）：`wifi_manager.StartConfigAp()` + `Alert(..., OGG_WIFICONFIG)` 播报 AP 名与网址（`wifi_board.cc:174-187`）
- `CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING`：`Blufi::GetInstance().init()`（`wifi_board.cc:188-192`）；该选项 select `BT_ENABLED/BT_BLE_42_FEATURES_SUPPORTED/BT_BLE_BLUFI_ENABLE`，采用 ESP-IDF 6 的 PSA Crypto BluFi（要求客户端支持 ffdhe3072/SHA-256/AES-CTR，Kconfig help 原文）

### 5.3 关键实现位置

- **AP + HTTP 配网页面不在本仓库**：`WifiManager/SsidManager/WifiStation` 均来自托管组件 `78/esp-wifi-connect: ~3.3.1`（`main/idf_component.yml:27`）。AP SSID 前缀 "Xiaozhi"（`wifi_board.cc:57`）；Web 路由、HTTP 接口需拉取组件后核对。
- **BluFi**（`main/boards/common/blufi.cpp`）：`init()`（blufi.cpp:103-120）启动 BLE 广播 + Wi-Fi 扫描；收到 `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP` → `SsidManager.AddSsid(ssid, password)`（blufi.cpp:866-900）→ 停 AP、起 station；连上网后 `Blufi::deinit()`（`wifi_board.cc:122-125`）。仓库 sdkconfig.defaults 中无板级强制开启 BLUFI——**默认全部走热点配网**。
- 配网页面进出通过 `NetworkEvent::WifiConfigModeEnter/Exit` 回调通知应用层（`wifi_board.cc:87-92,138-147`）。

### 5.4 配网成功路径

配网页面保存 SSID → `WifiConfigModeExit` → `TryWifiConnect()` 重新连网（`wifi_board.cc:142-147`）→ 连上 → 应用层 `HandleNetworkConnectedEvent()` → 进入 `Activating`（application.cc:302-317）→ 走第 3 节激活链。

> **对方案第五节的意义**：热点与 BluFi 两条配网方式、编译期开关、状态机解耦都是现成的；方案中的 `provisioning_manager`（统一入口/状态协调）属于**重构封装**而非从零实现。二维码配网当前完全不存在。

## 6. 提示音体系

### 6.1 文件清单（全部实际存在，.ogg）

- 公共（`main/assets/common/`）：`exclamation.ogg, low_battery.ogg, popup.ogg, success.ogg, vibration.ogg`
- 语言包（`main/assets/locales/<lang>/`，30+ 语言目录，en-US 为基准）：`0.ogg~9.ogg, activation.ogg, err_pin.ogg, err_reg.ogg, upgrade.ogg, welcome.ogg, wificonfig.ogg`

### 6.2 引用与播放链

构建：`scripts/gen_lang.py` 读 `assets/locales/<LANG_DIR>/language.json` → 生成 `main/assets/lang_config.h`（`main/CMakeLists.txt:1205-1218`）→ `Lang::Sounds::OGG_XXX` 为 `std::string_view`，绑定 `asm("_binary_<name>_ogg_start/_end")`（gen_lang.py:141-166）；ogg 经 `EMBED_FILES` **无条件嵌入固件**（`main/CMakeLists.txt:1126-1127`）。

运行：`AudioService::PlaySound(ogg)`（`main/audio/audio_service.cc:768-790`，OggDemuxer 解封装 → 解码队列）← 上层 `Application::PlaySound()`（application.cc:1441）与 `Application::Alert(status, message, emotion, sound)`（application.cc:765-775）。

### 6.3 调用点（现状全部调用）

| 声音 | 调用点 |
| --- | --- |
| `OGG_ACTIVATION` + 数字 0-9 | 激活码播报（application.cc:754,760） |
| `OGG_SUCCESS` | 激活完成（application.cc:373） |
| `OGG_WIFICONFIG` | 进入热点配网（wifi_board.cc:186） |
| `OGG_POPUP` | 通知前提示（application.cc:1207） |
| `OGG_VIBRATION` | 服务器 alert 下发（application.cc:706-712） |
| `OGG_LOW_BATTERY` | 低电量（`main/display/lvgl_display/lvgl_display.cc:274`） |
| `OGG_ERR_PIN/OGG_ERR_REG` | 4G SIM 错误（application.cc:156-167） |

> **对方案 4.2 的意义**：现状**没有任何提示音策略开关**；且声音经 `EMBED_FILES` 无条件打包。"关闭某类提示音且不打包资源"需同时改：① 调用点（加条件）② `EMBED_FILES` 列表（main/CMakeLists.txt:1126-1127）③ `gen_lang.py` 生成逻辑。另外 `FLASH_NONE_ASSETS` choice（Kconfig 9-26）是现成的"整体资源裁剪"机制，可作参照。

### 6.4 服务器下发音频的两条路径

1. 会话 TTS：`Protocol::OnIncomingAudio` → `audio_service_.PushPacketToDecodeQueue()`（application.cc:567-571；WS 二进制帧 `websocket_protocol.cc:108-140` 或 MQTT+UDP AES-CTR 解包 `mqtt_protocol.cc:302-369`）。
2. notify 推送：JSON `type=="notify"` 带 `audio_url` → `StartNotification()`（application.cc:628-631, 1182-1226）→ `notify_player_` HTTP 拉流播放（`main/notify/notify_player.h:28`）。

## 7. 协议层共享语义

- 基类：`main/protocols/protocol.h/.cc`。共享逻辑：`session_id_`（protocol.h:82）；`SendAbortSpeaking/SendWakeWordDetected/SendStartListening/SendStopListening/SendMcpMessage` 拼统一 JSON 后调纯虚 `SendText()`（protocol.cc:66-106）；120s 超时（protocol.cc:108-117）。
- 两协议对称实现 `OpenAudioChannel()`/`GetHelloMessage()`/`ParseServerHello()`（WS：`websocket_protocol.cc:79-266`；MQTT：`mqtt_protocol.cc:274-503`，MQTT 另建 AES-CTR 加密 UDP 通道）。
- 协议选择：OTA 下发了 mqtt 配置用 MQTT，否则 WebSocket（application.cc:551-558）。
- **认证方式**：WebSocket 头 `Authorization: Bearer <token>`（token 来自 NVS `"websocket"`，即服务端下发）；MQTT 用服务端下发的 username/password 连 broker。**设备侧不本地生成协议凭据**。

## 8. 待验证项（需联调/真机）

1. `78/esp-wifi-connect` 组件内 AP 配网页面的路由与接口（`managed_components/` 未拉取，无法核对）。
2. eFuse HMAC_KEY0 与 `ESP_EFUSE_USER_DATA` 序列号的**烧写流程**（固件侧只见读取，产测工具不在本仓库），v2 激活需真机验证。
3. `OGG_WELCOME` 符号由 gen_lang.py 生成，但未在 `.cc` 中发现调用点，疑为遗留资源或经其他路径使用，需全文核对。
4. 服务器实际响应字段（activation.code 位数、mqtt/websocket 对象字段名）以 `api.tenclass.net` 实测为准；本报告仅描述固件解析端约定。
5. BluFi PSA Crypto 安全协议需与配套 App 联调。
