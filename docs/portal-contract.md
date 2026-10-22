# 配网页契约（OEM 整页替换）

本文件是 `product/profiles/<tenant>.json` 中 `web.portal_pack` 整页替换（L3）的接口契约。
租户页面可以任意重写 HTML/CSS/JS，但必须遵守下列端点与语义，否则配网流程会失败。

设备在配网模式下开启 SoftAP（默认 `http://192.168.4.1`），由 `components/esp-wifi-connect`
提供 HTTP 服务。页面资源由固件在构建期内嵌，运行时不访问互联网。

## 端点

| 端点 | 方法 | 请求 | 响应 |
| --- | --- | --- | --- |
| `/` | GET | 无 | 配网主页 HTML（租户 `index.html` 或内置页） |
| `/brand.js` | GET | 无 | 仅当档案提供品牌配置时存在：`window.BRAND_CONFIG = {...};` |
| `/brand-logo` | GET | 无 | 仅当档案提供 logo 时存在：logo 原始字节（`svg`/`png`） |
| `/scan` | GET | 无 | `{"support_5g": bool, "aps": [{"ssid": str, "rssi": int, "authmode": int}, ...]}` |
| `/submit` | POST | JSON `{"ssid": str, "password": str}` | `{"success": true}` 或 `{"success": false, "error": str}` |
| `/saved/list` | GET | 无 | 已保存 SSID 字符串数组：`["ssid1", "ssid2"]` |
| `/saved/set_default` | GET | `?index=<n>` | 设为默认网络 |
| `/saved/delete` | GET | `?index=<n>` | 删除指定已保存网络 |
| `/advanced/config` | GET | 无 | 高级配置 JSON（OTA URL、TX 功率、记住 BSSID、睡眠开关） |
| `/advanced/submit` | POST | JSON（字段同上表） | 保存高级配置 |
| `/done.html` | GET | 可选 `?message=<text>` | 连接成功页（租户 `done.html` 或内置页） |
| `/exit` | POST | 无 | 退出配网模式 |

补充语义：

- `POST /submit` 成功后设备会保存 SSID 并尝试连接；前端应随后跳转
  `/done.html`（内置页由服务端在跳转时拼接 `message` 参数）。
- 内置页在提交成功后调用 `/done.html`，该页延迟 1 秒自动 `POST /exit`。
- 所有响应均为短连接（`Connection: close`），不要依赖 keep-alive。
- `authmode` 为 ESP-IDF `wifi_auth_mode_t` 数值；内置页据此判断是否显示密码输入。
- 5G 支持由 `CONFIG_SOC_WIFI_SUPPORT_5G` 决定，随 `/scan` 的 `support_5g` 返回。

## Captive portal 探测

设备对常见探测路径返回 `302` 到 `/?lang=<device language>`：

```
/hotspot-detect.html        Apple
/connectivity-check.html    Firefox
/hotspot-detect.html        Windows
/portal.html                Various
/library/test/success.html  Apple
```

自定义页面无需处理这些路径，但 **必须容忍 `?lang=` 查询参数**（忽略或据此选择文案）。

## 品牌钩子

- 页面若保留 `<script src="/brand.js"></script>`，可继续使用 L1/L2 的
  `window.BRAND_CONFIG`（`name`、`title`、`accent`、`theme`、`css_vars`、`i18n`、
  `sections`、`notice`、`footer`、`support_url`、`logo`）。
- 完全自定义页面可以忽略 `/brand.js`；此时 L1/L2 配置对页面无效。
- 内置页的 `applyBrand()` 逻辑可作为实现参考
  （`components/esp-wifi-connect/assets/wifi_configuration.html` 末尾）。

## 体积与编码

- 构建时页面会被预压缩为 gzip 并随固件下发（`Content-Encoding: gzip`），
  单页原始大小上限 512 KB；超过 256 KB 构建会告警。
- 页面资源必须内联在 HTML 中（`data:` URL 或内联 CSS/JS）；固件只提供上述端点，
  不会额外服务静态目录。
- 分区预算：多数布局 app 分区为 3–4 MB，页面包压缩后建议不超过 80 KB。

## 验收清单

- [ ] 连接热点后可打开 `/`，页面无外部网络依赖
- [ ] `/scan` 列表可刷新，选择 SSID 后可输入密码并 `/submit` 成功
- [ ] 提交失败时页面展示错误，不白屏
- [ ] `/done.html` 可访问并自动 `/exit`
- [ ] 手机/平板 captive portal 自动弹窗正常
- [ ] `?lang=xx` 不导致页面异常
- [ ] `main/boards/common/wifi_board.cc` 的 `BuildPortalBrandJson()` 输出与页面约定一致
