# lane-pairing-qr：wawabot-esp32 固件配对码二维码屏显

> 分支 `feat/lane-pairing-qr`（基于 main @ 0a09db5 的 worktree
> `.worktrees/lane-pairing-qr`）。api/mobile/server 仓全程只读。

## 1. 码体系核对结论（本 lane 最重要的核对点）

**激活码 ≠ 配对码，是两套完全独立的体系**，mobile QR 契约走的是第三条通道：

| | 激活码（activation） | 配对码（pairing） | QR 扫码（本 lane） |
| --- | --- | --- | --- |
| 用途 | 未注册设备注册认领（claim） | 已注册设备租户移交（redeem） | 消费端扫码绑定（bind-by-uid） |
| 形态 | **6 位**纯数字展示码（api `ActivationDisplayCodeLen=6`，`ota_activation.go:65`） | **8 位**纯数字（api `PairingCodeDigits=8`，`device_pairing.go:23`） | 无码，内容为 device_uid |
| TTL | 10 min（`ActivationTTL`） | 10 min（`PairingTTL`） | expire_at 省略 |
| 生成方 | OTA challenge 流程服务端签发（`/ota/check` v2 → `activationBlock{code,challenge,timeout_ms}`） | 控制台 `POST /api/v1/devices/pairing-codes`（服务端只存 SHA-256 摘要） | 设备本地离线生成 |
| 设备感知 | 屏显 + 语音播报（`ShowActivationCode`，固件已有） | **设备完全不感知** | 设备就是内容来源 |
| 兑换 | 控制台输码 + `/ota/activate` 轮询 | `POST /api/v1/device-pairing/redeem`（挂控制台成员面，APP 面 Bearer 无挂载） | mobile lane 提案 `bind-by-uid` |

固件现有的「屏上已有码」= 激活码（6 位，OTA `activation.code`），**不是**
mobile `parseConsumerQrScene` 要的 device_uid——把激活码塞进二维码 mobile 也
解析不出绑定语义。因此本 lane 的 QR 内容按 mobile 契约取 **device_uid**，
设备本地即可生成，**不需要任何新端点**；激活码继续走原有播报+屏显通道，两
通道并存（激活等待期是自然的扫码绑定时机，QR 叠加在激活屏显上）。

## 2. QR 内容格式对齐提案（跨仓契约，只提案不单方定稿）

固件实际下发（与 mobile `parseConsumerQrScene` 逐项对照）：

```
<Device-Id MAC>[,<BOARD_NAME>]
例（LCD/彩屏）: aa:bb:cc:dd:ee:ff,bread-compact-wifi
例（单色小屏）: aa:bb:cc:dd:ee:ff
```

- **device_uid**：`SystemInfo::GetMacAddress()`，与 OTA `Device-Id` 头逐字一致
  （小写冒号 MAC）——api `LocateForOTA` 正是用这个头寻址，保证三端同一键。
  mobile mock 的 `WAWA-ESP32-4821` 风格 uid 与真实固件值不同形，**mock 需按
  MAC 风格改**，否则真机联调时绑定面匹配不上。
- **device_type**：取 `BOARD_NAME`（kebab-case 板型）。语义待对齐：api
  `devices.device_type` 是管理员可设的产品类型字段（mobile mock 用
  `wawa-bot`），与 BOARD_NAME 值域不同。两个可选收敛方向（需 api/mobile
  lane 认领）：① mobile 把该字段当「型号」展示；② api 注册链路把 OTA 上报
  的 board 归一成 device_type。固件侧保持下发 BOARD_NAME，不加新 Kconfig。
- **expire_at**：省略（parser 得 0 = 长期有效、服务端终审）。理由：固件不知
  道任何配对窗口；激活 challenge 的 timeout_ms 是激活码语义，与 uid 绑定无
  关，不做耦合。若产品要求二维码本体过期：需 api 在 `/ota/check` 响应加
  扩展字段（如 `activation.qr_expire_at`），固件按「有则渲染、无则省略」
  消费——属兼容扩展，需三端提案共同定稿。
- 单色小屏省略 device_type 是**显示能力驱动的下发差异**：mobile parser 两字段
  均容缺省，无需分叉解析逻辑。

## 3. 码获取链路现状与待办

**现状**：固件 QR 完全本地生成（MAC + 板型），零新协议、零新端点、协议常量
零改动（`/xiaozhi/ota`、`Device-Id` 等未动）。渲染时机 = 激活等待期
（`CheckNewVersion` 内 `ShowActivationCode` 之后），设备已在屏、正等绑定。

**端到端待办**（服务端/APP 链路未就绪，固件侧已就绪）：

1. api：APP 面 uid 绑定端点（mobile lane 已提案 `POST /app/v1/devices/bind-by-uid`
   `{device_uid}`；现 `POST /app/v1/devices/bind` 只收 device_id UUID）。
2. api：目标租户 device_uid 冲突 → 409 语义沿用（`RedeemPairingCode` 同款）。
3. mobile：mock fixture 改 MAC 风格 uid；扫码后调 bind-by-uid 真实通道。
4. 联调登记：真机屏显 QR → 手机扫 → bind-by-uid → 设备列表出现该设备。

## 4. 实现落点（遵守仓内分层）

| 层 | 文件 | 内容 |
| --- | --- | --- |
| application | `main/application.cc/.h` | `ShowPairingQr()`：拼内容、选字段、调 Display；激活轮询结束/过期即收起 |
| ota | `main/ota.h` | `GetActivationTimeoutMs()` 访问器（字段已解析，此前未暴露） |
| display | `main/display/display.h` | `ShowQrCode/HideQrCode` 虚接口，默认 no-op（NoDisplay/EmoteDisplay 零影响） |
| display | `main/display/lvgl_display/lvgl_display.{h,cc}` | LvglDisplay 统一实现（OLED/LCD/E-Paper 共享）：全屏白底卡片 + `lv_canvas` I1 调色板自绘，模块缩放按屏幕短边自适应 |
| display | `main/display/qrcode/qrcodegen.{c,h}` | vendored Nayuki QR 编码器（MIT，取自 LVGL 9.5 组件），公共符号改前缀 `qr_`；宿主单测通过（version 3 / 29x29 / finder pattern 正常） |
| assets | `locales/{en-US,zh-CN,zh-TW}/language.json` | 新增 `ACTIVATION_EXPIRED`（其余 36 语言按 gen_lang.py 基准回落 en-US） |

**为什么不用 LVGL `LV_USE_QRCODE`**：`esp_emote_gfx` 组件内置一份未改名的
qrcodegen 且被 emote_display 无条件链接，`LV_USE_QRCODE=y` 直接多重定义链接
失败（bread-compact-esp32 实测）。vendored 前缀版绕开冲突，与仓内 gifdec.c /
jpeg_to_image.c 的 vendored 先例一致。

**过期/刷新行为（最小实现，沿用现有 UI 语言）**：以服务端 `timeout_ms`
（缺省 30s）为窗口，轮询中到期 → `SetChatMessage(ACTIVATION_EXPIRED)` +
跳出轮询 → 外层既有循环重跑 `CheckVersion` 取新码重渲染（音频上表现为
OGG_ACTIVATION + 数字播报重放，与现状一致）。轮询成功/被唤醒打断/过期三
条退出路径都会 `HideQrCode()`。

## 5. 构建证据

```
$ python3 scripts/build.py bread-compact-esp32 --name bread-compact-esp32   # OLED 128x64, esp32
build.py exit=0
build/xiaozhi.bin = 1,939,616 bytes（ESP-IDF v6.1）

$ python3 scripts/build.py kevin/c3                                        # 无屏, esp32c3
build.py exit=0
build/xiaozhi.bin = 1,714,400 bytes（ESP-IDF v6.1）
```

宿主侧：`python3 -m unittest discover -s scripts/tests` → Ran 183, FAILED
(errors=8)——**8 个错误在干净 HEAD（0a09db5）上同样存在（stash 对照验证），
本 lane 零新增失败**。

## 6. 真机待办清单（无烧录条件，构建 + 代码走查即止）

1. bread-compact-wifi / bread-compact-esp32（SSD1306 128x64）真机烧录：激活
   流程 QR 实扫——25 模块 @2px/模块（约 0.38mm/模块），近距离可扫但**对比度/
   对焦余量偏紧**，不达预期时的候选缓解：缩短内容（已是 uid-only）、状态栏
   临时避让换取更大 canvas、或换 0.96" 以上屏。
2. 128x32 变体（bread-compact-esp32-128x32）：v2 码只放得下 1px/模块，预期
   **不可扫**——产品侧需确认该板型不做扫码绑定（固件行为：渲染 1px 版本并
   LOGW，不崩溃、不阻塞激活）。
3. LCD 板型（如 guition-jc4827w543）真机验证 overlay 层级/主题对比度、
   E-Paper 板型验证刷新体验。
4. 激活窗口过期的「过期提示→重取」节奏真机感受（服务端 TTL 10min 时窗口内
   不会触发，触发场景主要是缺省 30s 的上游服务器）。
5. Wake word 打断激活播报期间 QR 状态（现有逻辑沿用，未新增交互面）。

## 7. 合并注意事项

1. **vendored 文件**（`main/display/qrcode/qrcodegen.{c,h}`）改动仅限：符号
   前缀 `qr_`、去 LVGL 头/宏依赖、assert 化。后续同步 LVGL 组件时若上游
   qrcodegen 升级，需重做同样变换（文件头有注释记录来源）。
2. `main/CMakeLists.txt` 新增 1 源文件 + 1 include dir；`sdkconfig.defaults`
   **零改动**（方案改 vendored 后不需要 `LV_USE_QRCODE`，无全板 flash 回归）。
3. 语言文件仅 en-US/zh-CN/zh-TW 各 +1 行；其余语言构建期自动回落基准，
   不需要补翻译（要补的话 key = `ACTIVATION_EXPIRED`）。
4. 新增 `Display::ShowQrCode/HideQrCode` 是基类默认实现（非纯虚），第三方
   自定义板型子类不实现也不受影响。
5. mobile 侧两处对齐项见 §2（mock uid 风格、device_type 语义）与 mobile lane
   提案 `.progress/lane-w1m2-consumer-device.md` 呼应；api 侧缺口见 §3。
6. 本分支基于 main @ 0a09db5，未 push、未 merge；`build/`、
   `managed_components/` 未入库。
