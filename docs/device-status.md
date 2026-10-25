# Device Status JSON（`self.get_device_status` 输出契约）

设备侧 MCP 标准工具 `self.get_device_status`（`main/mcp_server.cc` 的
`AddCommonTools` 注册）返回 `Board::GetDeviceStatusJson()` 的输出。服务端
（wawabot-server 设备状态链）在设备 MCP 工具聚合完成后调用该工具采集状态，
本文是该输出的固件侧权威说明，供 server / api / APP 读侧对照。

## WifiBoard（WiFi 板，`main/boards/common/wifi_board.cc`）

```json
{
    "audio_speaker": { "volume": 70 },
    "screen": { "brightness": 100, "theme": "light" },
    "battery": { "level": 85, "charging": false },
    "network": {
        "type": "wifi",
        "ssid": "MyWiFi",
        "rssi": -58,
        "signal": "strong"
    },
    "chip": { "temperature": 42.5 }
}
```

| 键 | 类型 / 单位 | 缺省行为 | 说明 |
| --- | --- | --- | --- |
| `audio_speaker.volume` | int，0-100 | 对象恒在；无 codec 时 `volume` 键省略 | 当前输出音量 |
| `screen.brightness` | int，0-100 | 无背光时省略 | 屏幕亮度 |
| `screen.theme` | string | 无 display（height≤64）或无主题时省略 | LVGL 主题名 |
| `battery.level` | int，0-100（百分比） | **无电池板整个 `battery` 对象省略** | 板级 `GetBatteryLevel()` 返回 false 即省略 |
| `battery.charging` | bool | 同上 | 是否充电中 |
| `network.type` | string，恒 `"wifi"` | 恒在 | 网络类型 |
| `network.ssid` | string | 恒在（未连接可为空串） | 当前 SSID |
| `network.rssi` | **int，dBm（负数，如 -58）** | **未连接或查询失败时省略** | 原始信号强度，取自 `esp_wifi_sta_get_ap_info`（经 `WifiManager::GetRssi`）；该 API 不可得时返回 0 哨兵，而 0 dBm 物理上不可能出现，故 0 视为不可得、不出键。读侧按缺键处理，不要造默认值 |
| `network.signal` | string，`strong`/`medium`/`weak` | 恒在 | 档位，由 rssi 派生：≥-60 strong、≥-70 medium、否则 weak（未连接时 rssi 哨兵 0 落入 strong，历史行为，勿依赖） |
| `chip.temperature` | float，°C | 无温度传感器时整个 `chip` 对象省略 | SoC 温度 |

**兼容性约定**：读方按"缺键 = 未知"逐键取值，未知键忽略——本仓对输出加键
（如 `network.rssi`）属于向后兼容扩展，不改既有键名与类型，不破坏旧读方。

## 其他板型的 `network` 形状（对照，读方按 `network.type` 分支）

| 板型 | network 键 | 说明 |
| --- | --- | --- |
| ML307 / 蜂窝板（`ml307_board.cc`、`nt26_board.cc`） | `type:"cellular"`、`carrier`、`csq`（int，AT+CSQ 值，0-31/99） | 蜂窝信号无 WiFi dBm 概念，**无 `rssi` 键** |
| 以太网板（`ethernet_board.cc`） | `type:"ethernet"`、`ip`、`mac` | 有线接入，**无 `rssi` 键** |
| RNDIS / 双网板 | 各自实现见对应 `*_board.cc` | `DualNetworkBoard` 委托当前活动网络板 |

## 与 hello 的关系

hello 消息（`main/protocols/websocket_protocol.cc` / `mqtt_protocol.cc`）**不携带**
状态字段，仅有 `version` / `features` / `audio_params`（外加 `transport` 语义）。
设备状态的唯一采集通道是 MCP 工具调用 `self.get_device_status`：每会话一次，
在设备侧 MCP 工具聚合完成后由服务端发起。会话内状态变化对服务端不可见
（周期重报见 known-gaps F45 ③，固件侧定时器尚未实现）。
