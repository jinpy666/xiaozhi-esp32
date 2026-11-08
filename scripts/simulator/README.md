# 宿主 UI 模拟器(ui_sim)

把「我们改造过的固件」在 Mac 上以**窗口/截图**形式模拟出来:M2 档——复用本仓自己的
LVGL 9.5(managed_components)真渲染 UI 布局,按真实固件的 Display 调用序列驱动,
无板卡、无 QEMU、秒级构建。

## 定位与保真边界

| 真实的 | 模拟的 | 说明 |
| --- | --- | --- |
| LVGL 9.5 + 布局骨架 | ✅ 同一份 LVGL 源码;布局复刻自 `LcdDisplay::SetupUI`(lcd_display.cc) | 状态栏/通知互斥、表情区、聊天气泡同构 |
| `Display::` 事件序列 | ✅ 每个场景逐条对齐 application.cc 真实路径(见下表) | 状态文本用 Lang::Strings 真值 |
| 固件编译(application.cc 等) | ❌ 不编译固件 | 那是 M3(linux target)的范围 |
| emoji/图标字体 | 文本占位(`[ neutral ]`) | 字体资产宿主不可用 |
| 音频/振铃 | 无 | 语音面必须真机 |

## 场景 ↔ 固件路径对照

| 场景 | 固件路径 | 屏上表现 |
| --- | --- | --- |
| `idle` | `OnStateChanged(kDeviceStateIdle)`(application.cc:904) | Standby + neutral |
| `incoming` | remote_wakeup→Connecting→Listening;TTS 直读播报(HandleFamilyCallIncoming, application.cc:1954;InjectFamilyCallAnnouncement, :2005)| Connecting...→Listening...,助手气泡「您收到来自 妈妈 的来电」,用户 STT 气泡 |
| `active` | 桥接通话中的 Speaking/Listening 循环 | 状态栏交替 |
| `hangup` | goodbye 对端挂断 | 助手气泡「对方已挂断」→ Standby + neutral |

## 用法

```bash
# 编译(首次约 30s,产出 build/ui_sim)
bash scripts/simulator/build_ui_sim.sh

# 弹出 SDL 窗口看单个场景(窗口模式)
build/ui_sim --scene incoming

# 无头批量截图(CI/汇报用,输出 BMP;sips 可转 PNG)
build/ui_sim --all --headless --shot /tmp/ui
sips -s format png /tmp/ui-incoming.bmp --out /tmp/ui-incoming.png
```

`--scene idle|incoming|active|hangup`,`--all` 全部场景依序截图。

## 已知边界

- 状态文本为默认 locale(英文)真值;中文 locale 由固件按激活地区切换,值同源不同文案。
- 中文气泡用 LVGL 自带思源黑体 16px 常用字集(`lv_font_source_han_sans_sc_16_cjk`),子集外
  字符显示为方块(实测「收 / 媛 / 听」等不在子集);全量中文需接 `78/xiaozhi-fonts` 的
  cbin 字体资产(后续可做)。
- `ShowNotification` 的超时回切依赖 esp_timer,模拟器里为常显(手动切换场景)。
- 表情/图标字体未接,以文本占位。
- BMP 截图可 `sips -s format png x.bmp --out x.png` 转 PNG。
