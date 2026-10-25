# lane-status-fields：固件设备状态字段补齐（known-gaps F45 ①②）

> 分支 `feat/lane-status-fields`（基于 main @ 04f3e9a 的 worktree
> `.worktrees/lane-status-fields`）。server / api 仓全程只读。
> 对应 server 仓 `docs/known-gaps.md` **F45 ①（hello 字段）与 ②（原始 RSSI）**。

## 1. 与 server 解析的对齐结论（契约真相）

server 侧消费面逐字核对（wawabot-server main @ cec8554）：

- **状态采集唯一通道**：`internal/pipeline/devicestatus.go` 的
  `collectDeviceStatus` —— 设备 MCP 工具聚合完成后调用
  `self_get_device_status`（固件 `self.get_device_status`），经
  `parseDeviceStatusJSON` 逐键解析后上报 `device.status` / `device.properties`。
- **`parseDeviceStatusJSON` 消费的键**（指针字段、缺键即未知，不造默认值）：
  `battery.level`（float）、`battery.charging`（bool）、
  `network.type` / `network.signal` / `network.state`（string）、
  `chip.temperature`（float）。
- **rssi 接纳方式**：F45 明示「固件补齐后 server 只需在 parseDeviceStatusJSON
  加键，wire 契约向后兼容」。本仓加 `network.rssi` 键不改既有键，server 现版本
  忽略未知键不受影响；server 侧加 `Network.Rssi *int \`json:"rssi"\`` 一行 +
  `status[PayloadKey...]` 一行即消费（api 侧 `device_property_latest` 投影自动
  带上，`scalarProperties` 收 int→float64 无障碍——PropertySink 落库口径见
  server `internal/controlplane`）。
- **hello 消费面**：`internal/protocol/messages.go` 的 `Hello` 结构只绑定
  `version` / `transport` / `audio_params` / `features` 四键，`ParseIncoming`
  二次绑定丢弃其余；`session.applyHello` 只做 transport 校验、音频参数协商、
  features 记录。**hello 里的任何状态键今天是死负载**。
- **api 侧 rssi 的另一条路**：`wawabot/runtime/protocol/xiaozhi_adapter.go`
  只从 WS 升级头 `X-Wifi-Rssi` 取 rssi（ElatoAI 类第三方固件的扩展头，xiaozhi
  固件不发），缺失即 `hasRSSI=false`、心跳载荷不出 rssi 键。固件侧 MCP 工具
  提供 rssi 后，APP `wifi_rssi` 的持久来源仍走 device.properties 投影，
  `X-Wifi-Rssi` 通道不受影响。

## 2. 交付 A：GetDeviceStatusJson 增 network.rssi（已实现）

`main/boards/common/wifi_board.cc` `WifiBoard::GetDeviceStatusJson()`，Network
段在既有 signal 档位旁增：

```cpp
int rssi = wifi.GetRssi();
if (rssi != 0) {
    cJSON_AddNumberToObject(network, "rssi", rssi);   // 原始 dBm，负数
}
const char* signal = rssi >= -60 ? "strong" : ...;    // 档位逻辑零改动
```

- **取值路径**：`WifiManager::GetRssi()` → `WifiStation::GetRssi()` →
  `esp_wifi_sta_get_ap_info`（`components/esp-wifi-connect/wifi_station.cc:334`）。
  未连接或查询失败时该 API 返回 0 哨兵——0 dBm 物理上不可能，故 0 即"不可得
  不出键"，与 server「缺键按未知」口径闭环。无电池板行为（battery 对象整段
  省略）不回归：battery 段逻辑零改动。
- **与 signal 档位共存**：rssi 是新增键，signal 派生表达式未动（含未连接时
  哨兵 0 落 strong 的历史行为——只写实、不顺手修，见 §6）。
- **板型范围**：仅 WifiBoard。蜂窝板（ml307/nt26）用 `csq`、以太网板
  `type:"ethernet"`，无 WiFi dBm 概念，不发明键；`DualNetworkBoard` 委托
  活动板自动继承。`self.get_device_status` 是 `Board` 虚方法，逐板实现。
- **不触碰**：`components/`（vendored，AGENTS.md 红线）、hello/`/xiaozhi/v1/`/
  `/xiaozhi/ota`/`xiaozhi.audio_params.*`（协议红线，零改动）。

## 3. 交付 B：hello 增状态字段——**结论：不加**

按 lane 指令"先读 server 解析再决定、不盲目加"，核查结论：**server 在 hello
里不解析任何状态键，只从 MCP 工具取**（§1 证据链）。hello 加 battery/wifi
对象是 wire 兼容的（server 丢弃未知键），但没有消费者的字段是死负载：

1. **无读方**：`protocol.Hello` 不绑定、`applyHello` 不消费、
   `parseDeviceStatusJSON` 收不到 hello——数据到 server 即丢。
2. **双传输成本**：hello 在 websocket 与 mqtt 两协议各构造一次
   （websocket_protocol.cc:211 / mqtt_protocol.cc:390），AGENTS.md 要求
   共享语义两传输同改同验，改动面翻倍而收益为零。
3. **单一事实源**：现状态链"每会话 MCP 工具取一次"已是唯一通道；hello 再带
   一份会形成第二来源，两处读数时点不同（hello 早于 MCP 就绪数秒），
   server 侧还要先定优先级口径——为省一次 10s 预算内的工具往返，不值。
4. **连接关键路径加 IO**：hello 构造在拨号路径上，电池读数（I2C/ADC）与
   RSSI 查询塞进 hello 会加拨号时延，违背"采集是尽力而为旁路"的 server 侧
   既有设计。

**server 侧待办（启用 hello 字段的前置条件，若"零 RTT 取态"真有需求）**：
① `protocol.Hello` 增可选 `Battery`/`Network` 指针结构（键形与
`parseDeviceStatusJSON` 完全一致，复用键名）；② `applyHello` 或 pipeline 在
hello 阶段合成一条 `device.status` 观测（与 MCP 工具观测同载荷形状，
标注来源字段区分新鲜度）；③ 此后固件才有加键的意义（websocket + mqtt 两处
SendHello 同步加 `battery{level,charging}` + `network{rssi}`）。在此之前，
F45 ①的固件侧动作维持"不做"。

## 4. 交付 C：构建验证（证据）

环境：ESP-IDF release/v6.1（`/Users/Jinpy/esp/esp-idf`，source export.sh），
`python3 scripts/build.py <board-dir> --name <variant>`。

| 板型 | 特征 | 结果 |
| --- | --- | --- |
| `espressif/esp32-s3-korvo-2-v3.0` → `esp32s3-korvo2-v3` | 带电量（PowerManager）+ 带屏 | exit=0 |
| `doit-s3-aibox` | 无屏、无电池（GetBatteryLevel 未覆写） | exit=0 |

（构建日志 `/tmp/build-korvo2.log`、`/tmp/build-doit.log`；`doit-s3-aibox` 无
battery 对象的行为是 wifi_board.cc 既有 `if (board.GetBatteryLevel(...))` 分支的
运行时行为，本次未触碰该段，编译面由双板构建覆盖。）

构建记录（2026-10-25，IDF release/v6.1，source export.sh 后）：

```
# 板 1：espressif/esp32-s3-korvo-2-v3.0（带电量 PowerManager + 带屏）
$ python3 scripts/build.py espressif/esp32-s3-korvo-2-v3.0 --name esp32s3-korvo2-v3
[100%] Linking CXX executable xiaozhi.elf
Project build complete. To flash, run: ...
EXIT=0                       # build/xiaozhi.bin 2729376 bytes

# 板 2：doit-s3-aibox（无屏、无电池，GetBatteryLevel 未覆写）
$ python3 scripts/build.py doit-s3-aibox --name doit-s3-aibox
Project build complete. To flash, run: ...
EXIT=0                       # build/xiaozhi.bin 2009040 bytes

# 格式：llvm@22 clang-format --dry-run -Werror 对 main 原版与本版同为 35 处
# 既有违规（版本漂移，非本次引入）；格式化输出 diff 仅含本次新增 7 行，
# 新增行被 clang-format 原样保留 = 零新增违规，未做全文件重排。
```

真机行为验证归 G3 口径（无设备环境）：连接态 rssi 键出现、断连态键消失、
`esp_wifi_sta_get_ap_info` 失败路径不出键，均需真机抓 `self.get_device_status`
应答确认。

## 5. 交付 D：文档

- 新增 `docs/device-status.md`：`self.get_device_status` 输出契约——WifiBoard
  全键形/类型/单位/缺省行为表、`network.rssi` 的 0 哨兵语义、其他板型
  （cellular/ethernet）network 形状对照、与 hello 的关系（不携带状态字段）。
- `docs/mcp-usage.md` 内置工具表下补一行指向该文档。

## 6. 已知边界与不顺手修的项

- **未连接时 signal 恒 strong**：`GetRssi()` 哨兵 0 落入 `>=-60` 分支，断连态
  档位失真。历史行为，属档位派生逻辑而非本 lane 范围，只写实不修（server 读
  侧应优先信 `rssi` 数值 + `network.state`，不信断连态的档位）。
- **`network.state` 键 server 在解析但 WifiBoard 从不出**：server
  `parseDeviceStatusJSON` 有 `Network.State` 分支，wifi 板实现里无此键（蜂窝/
  以太板亦无）——按缺键处理，不发明。
- **F45 ③（会话内低频重报的固件定时器）不在本 lane**：未动。
- F45 解除条件还差：server 侧 `parseDeviceStatusJSON` 加 rssi 键（一行）+
  ①/③ 的去留按 §3 论证定夺——本 lane 只交固件侧 ②，①按论证"不加"。

## 7. 合并注意事项

- 变更面：`main/boards/common/wifi_board.cc`（+6 行）、`docs/device-status.md`
  （新增）、`docs/mcp-usage.md`（+2 行）、`.progress/lane-status-fields.md`。
  无 Kconfig/CMake/组件改动，无 sdkconfig 影响，OTA 兼容性不受影响（板型
  标识未动）。
- server 合并次序无硬依赖：固件先发、server 后加解析键（旧 server 忽略新键）；
  反之 server 先合、旧固件不出键（指针字段缺键即未知），两个方向都向后兼容。
- 真机联调时用 `wawabot-bench` 或直接抓 `self.get_device_status` 应答，核对
  `network.rssi` 为负数 dBm 且与 `signal` 档位一致（rssi≥-60 ⇒ strong）。
