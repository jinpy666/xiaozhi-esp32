# Lane 报告：lane-family-esp32——FAMILY-PROTOCOL v1 设备面（§1 全量）

- 日期：2026-10-25；分支 `feat/lane-family-esp32`（rebase 后基于 main @ b8ab2a6），
  **已推送，main 合并挂起**（本机无 ESP-IDF，见门禁节）。
- 任务：FAMILY-PROTOCOL v1 设备泳道——传音录音上行、定位周期上报、SOS
  长按触发、四类下行处理。规格唯一真相源：wawabot-api 仓
  `app/wawabot/docs/FAMILY-PROTOCOL.md` §1（冻结）。
- 缘起：中断现场抢救——未提交改动停在 main @ 0a09db5 工作区（Kconfig +
  application.cc/h + protocol.cc/h，+638 行），抢救为分支后 rebase 干净
  （远端 pairing-qr/status-fields 两 lane 与本 lane 零冲突）。

## 交付物

- `main/protocols/`：`type:"family"` 信封；`SendFamily*` 上行族
  （VoiceMessage/LocationReport/Sos/MessagePlayed/Ack）统一走 `SendText()`
  ——WebSocket 文本帧 / MQTT JSON 发布两种传输零特化；`DispatchFamilyMessage`
  校验下行 action 后转发 `OnFamilyMessage` 注册方。
- `main/application.*`：传音录音生命周期（opus 帧本地缓冲不进对话管线，
  `CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS` 上限自动停+发）、SOS 长按
  （`CONFIG_FAMILY_SOS_GPIO`，-1 关闭）、`SetFamilyLocationProvider` 注入式
  定位源 + 周期上报（无源永不报）、下行四处理：message_arrived（拉
  audio_url 播放，不支持拉流回 `playback_unsupported`）、sos_acked（播报
  "家人已收到"）、alarm_fire、call_incoming（回 `not_implemented`——v1
  预留口径）。
- `main/Kconfig.projbuild`：录音时长上限（缺省 60s）/ 定位周期（缺省 5min）/
  SOS GPIO 三项。

## 门禁

- 2026-10-25 补：CI 钉版镜像 `espressif/idf:release-v6.1` Docker 化实机构建
  （named volume `wawabot-esp32-work` 缓存工具链，与 build.yml 同口径）：
  `scripts/build.py bread-compact-esp32` → BOARD1_BUILD_OK（esp32 + OLED）、
  `scripts/build.py kevin/c3` → BOARD2_BUILD_OK（esp32c3 无屏）——**双板全绿，
  main 合并解除挂起并已合入**。
- 对端泳道状态：server 侧已合入远端 main（lane-family-srv）；api 侧
  MD-32/33 已在远端 master；mobile 侧 feat/lane-family-mobile 已推分支。

## 遗留 / 真机项

- 真机四类用例（录音上行/定位上报/SOS 长按/到货留言播放）随 G3 真机清单
  一并执行；OLED 端 family 状态提示（录音中/SOS 已确认）未做 UI，属后续
  体验 lane。
