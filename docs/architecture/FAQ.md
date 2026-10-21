# FAQ（常见疑问速查）

> 条目均经源码核实并标注出处；与各章文档交叉引用。持续追加。

## Q1：设备会上报位置、IP、日志之类的吗？

**结论：位置不上报；IP 会上报；日志不上传。**

- **定位**：全仓库无 GPS/GNSS/经纬度相关代码，设备没有任何主动上报位置的通道。但以下数据可被服务端**间接**推断位置：WiFi SSID（网络名）、公网 IP（TCP 层天然可见）、4G 版上报的运营商与基站注册状态。
- **IP**：每次开机 CheckVersion 的上报 body 中，`board` 段明确包含 **`ssid / rssi / channel / ip`（局域网 IP）/ mac**（`main/boards/common/wifi_board.cc:265-279`）；配网模式下该段只保留 mac。公网 IP 则在 OTA、WebSocket/MQTT、Notify 拉流等所有连接中服务器自然可见。
- **日志**：全仓库无日志上传、崩溃上报、telemetry 通道（`log_upload / crash_report / sentry / telemetry / coredump` 全仓搜索为空）。`application.elf_sha256` 随 OTA 请求上报，但那是构建指纹（服务端识别构建用），不是日志。唯一的调试外发 `USE_AUDIO_DEBUGGER` **默认关闭**（Kconfig.projbuild:1048-1052），且只发原始 PCM 到局域网 UDP 地址（默认 192.168.2.100:8000），不经过云端。

### 设备对外发送数据清单

| 时机 | 发送内容 | 敏感度 |
| --- | --- | --- |
| 每次 CheckVersion（开机一次） | MAC、UUID、语言、固件版本/编译时间/elf_sha256、芯片型号、分区表、屏幕规格、**SSID/RSSI/信道/局域网 IP** | 中（WiFi 板，`board.cc:101-172` + `wifi_board.cc:265-279`） |
| 同上（**4G 版**，`ml307_board.cc:170-181`） | 上述之外还上报 **IMEI、ICCID（SIM 卡号）、运营商、模组版本、信号 CSQ、注册状态 cereg** | **高**——IMEI/ICCID 是设备级唯一标识 |
| 会话中 | 语音（Opus 上行，产品核心功能）；唤醒词文本 + 唤醒前音频片段（`SEND_WAKE_WORD_DATA` **默认开启**，仅 AFE/自定义唤醒词机型，Kconfig.projbuild:1001-1004） | 高 |
| 拍照问答（LLM 调用 `self.camera.take_photo`） | 照片 JPEG 上传到服务器下发的 vision URL | 高（需 LLM 主动调用） |
| LLM 查询 `self.get_device_status` 时 | 音量/亮度/主题/电量/网络类型/SSID/信号/芯片温度（按需，`wifi_board.cc:299-345`） | 低 |
| 日志 / 崩溃 / 遥测 | **无通道，不发送** | — |

### 隐私相关开关与裁剪出口

| 项 | 方式 |
| --- | --- |
| 唤醒词数据上传 | Kconfig `SEND_WAKE_WORD_DATA` 关闭（产品配置层可直接控制） |
| 上报字段裁剪 | 覆写 `GetBoardJson()`（虚函数，机制现成，全仓暂无覆写先例）——可删 ssid/ip/rssi 等字段 |
| IMEI/ICCID（4G） | 同样在 `Ml307Board::GetBoardJson()` 内，需覆写裁剪 |
| 自有服务端 | 以上全部字段由自有服务端定义，见 [11-server-protocol-spec.md](11-server-protocol-spec.md) |

## Q2：怎么换成自有服务器？

最小改动：`CONFIG_OTA_URL` 默认值（`main/Kconfig.projbuild:3-7`）或运行期 NVS `wifi.ota_url`。协议不变时设备侧无需其他改动；会话凭据由 CheckVersion 响应下发（mqtt/websocket 二选一）。完整线缆级规格见 [11 章](11-server-protocol-spec.md)。

## Q3：关闭激活语音提示会影响激活吗？

不会。激活业务（轮询 `/activate`）与反馈（播报数字）在代码上是分离的（application.cc:522-540 vs 741-763），关提示音只改反馈通道。但提示音裁剪要同时改三处：调用点、`EMBED_FILES`、`gen_lang.py`——见 [04 章 §3](04-feature-trimming.md)。

## Q4：WebSocket 断线会自动重连吗？

不会。WS 无自动重连，靠下次按键/唤醒重开通道（失败回 Idle）；MQTT 控制连接断开后 60 秒定时重连，且**仅 Idle 态执行**。详见 [07 章 §6](07-communication-protocol.md)。

## Q5：MCP 工具执行有超时吗？

没有。工具回调经 `Schedule` 在主任务**串行**执行；阻塞型硬件动作必须"回调入队 + 独立任务执行"（otto 模式），并配急停工具。见 [09 章](09-mcp-and-board-api.md)、[10 章 §4](10-hardware-extension-points.md)。

## Q6：仓库有顶层 config.json 吗？

没有。配置是每板一份 `main/boards/**/config.json`（157 份 / 191 个构建变体 / 7 个芯片目标），由 build.py 运行时收集。见 [02 章](02-build-pipeline.md)。

## Q7：怎么新增一块板？

六层清单（板目录 config.h/board.cc/config.json + Kconfig + CMakeLists + 可选专属源文件）见 [03 章 §5](03-board-identification.md)；板类结构与 DECLARE_BOARD 机制见 [09 章 §5](09-mcp-and-board-api.md)。

## Q8：设备身份和凭据是怎么管理的？

设备标识 = MAC + UUIDv4（NVS 持久化，可选 eFuse 序列号）；通信凭据（MQTT 用户名密码 / WS token）**全部由服务器经 CheckVersion 下发**写入 NVS，设备不本地计算；v2 激活用 eFuse HMAC 挑战应答；功能授权体系不存在。见 [03 章 §2-3](03-board-identification.md)、[06 章 §3](06-provisioning-and-activation.md)。

## Q9：上游同步现状？

`origin` = jinpy666 fork（GitHub fork 关系已在），本地**未配置 upstream remote**；基线 `aab98ab` 与上游 main 一致（零定制）。同步流程建议见 [../baseline-review/04-ci-workflows.md §1](../baseline-review/04-ci-workflows.md)。
