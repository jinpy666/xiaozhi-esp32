# Product Branding

本目录集中管理"构建时品牌化"：换品牌只改这里的文件，不改上游代码；上游同步后用审计脚本检查品牌残留。

## 1. 项目名与版本：`brand.cmake`

根 `CMakeLists.txt` 读取本目录的 `brand.cmake`：

```cmake
set(BRAND_PROJECT_NAME "xiaozhi")   # 应用描述符 project_name，影响 OTA 上报
set(BRAND_PROJECT_VER "2.5.3")      # 应用版本，必须为 x.y.z 数字格式
```

- 修改后重新构建即生效；`scripts/release.py` 会自动跟随这两个值（项目名从根 `CMakeLists.txt` / `brand.cmake` 动态解析，不再硬编码 `xiaozhi`）。
- 改项目名属于破坏 OTA 兼容性的产品级变更，发布前请确认你的 OTA 服务端接受新名字。
- `scripts/build.py` 的版本探测读取根 `CMakeLists.txt` 的 `set(PROJECT_VER ...)`；品牌版本以 `brand.cmake` 为唯一事实来源，`release.py prepare` 优先使用它。

## 2. 配网品牌项：`main/Kconfig`

`main/Kconfig` 定义可覆盖的品牌符号（均有中性默认值，不设置即维持原样）：

| Kconfig 符号 | 用途 | 默认值 |
| --- | --- | --- |
| `CONFIG_PRODUCT_BRAND_AP_PREFIX` | Wi-Fi 配网 AP SSID 前缀 | `Xiaozhi` |
| `CONFIG_PRODUCT_BRAND_BLE_NAME` | BluFi 蓝牙设备名 | `Xiaozhi-Blufi` |
| `CONFIG_PRODUCT_BRAND_NAME` | 配网页品牌名（logo alt 文本） | 空 |
| `CONFIG_PRODUCT_BRAND_PORTAL_TITLE` | 配网页浏览器标题 | 空 |
| `CONFIG_PRODUCT_BRAND_PORTAL_ACCENT` | 配网页主题色 `#RRGGBB` | 空 |
| `CONFIG_PRODUCT_BRAND_PORTAL_THEME` | 强制主题 `dark`/`light`/空=跟随系统 | 空 |
| `CONFIG_PRODUCT_BRAND_PORTAL_FOOTER` | 配网页页脚文案 | 空 |
| `CONFIG_PRODUCT_BRAND_PORTAL_SUPPORT_URL` | 配网页客服/帮助链接 | 空 |
| `CONFIG_PRODUCT_BRAND_LOGO_*` | logo 格式三选一（none/svg/png） | none |
| `CONFIG_PRODUCT_BRAND_PORTAL_CONFIG` | 嵌入租户声明式配置 JSON | n |

在产品板 `config.json` 的 `sdkconfig_append` 中覆盖，构建脚本零改动：

```json
{
    "builds": [
        {
            "name": "my-product",
            "sdkconfig_append": [
                "CONFIG_PRODUCT_BRAND_AP_PREFIX=\"MyBrand\"",
                "CONFIG_PRODUCT_BRAND_BLE_NAME=\"MyBrand-Blufi\""
            ]
        }
    ]
}
```

批量品牌化请改用下文的租户档案（`--brand`），不要逐板手写。

## 3. OTA URL

`CONFIG_OTA_URL` 的上游默认值指向 `https://api.tenclass.net/xiaozhi/ota/`。换品牌时必须在产品板 `config.json` 中覆盖：

```json
"sdkconfig_append": ["CONFIG_OTA_URL=\"https://your.domain/ota/\""]
```

审计脚本刻意**不**把该默认值列入白名单：未覆盖时会持续出现在品牌残留清单里作为提醒。

## 4. 唤醒词与提示音

- 唤醒词：使用 `python3 scripts/build.py <board> --wake-word <name>` 选择（`--list-boards` 旁可查可用项），详见 `docs/architecture/12-debranding-customization-plan.md`。
- 提示音：替换 `main/assets/` 下音频需保持 **Opus-in-Ogg、单声道**，并注意对应分区大小；格式不符会导致播放失败或资产打包超限。

## 5. 品牌审计

```sh
# 扫描 main/、根 CMakeLists.txt、scripts/release.py、scripts/versions.py
python3 scripts/product/brand_audit.py

# 扫描固件二进制（strings 等价提取）
python3 scripts/product/brand_audit.py --binary build/<board>/xiaozhi.bin

# 机器可读输出（CI 友好）
python3 scripts/product/brand_audit.py --json
```

- 模式：`(?i)xiaozhi|tenclass`。
- 白名单：`product/brand-audit-allowlist.txt`，每行一条正则，命中行文本或文件路径任一即忽略（`#` 为注释）。
- 有非白名单残留时退出码 1，否则 0，可直接接入 CI。

## 6. 上游同步检查

```sh
bash scripts/product/check_upstream.sh [upstream-url] [branch]
# 默认 https://github.com/78/xiaozhi-esp32.git main
```

- 只读操作：`git fetch` 到 `FETCH_HEAD`，用 `merge-base` 对比 `BASE..FETCH_HEAD`，列出触碰品牌敏感文件（根/`main` 的 CMake 与 Kconfig、`wifi_board.cc`、`blufi.cpp`、`release.py`、`build.py`、`versions.py`、`main/assets/`）的上游提交。
- 有触碰 → 退出码 1，提示复查品牌补丁并重跑 `brand_audit.py`；否则 0。不新建分支、不改工作区。

## 7. CI

`.github/workflows/brand-check.yml` 在 push / pull_request 时自动跑审计；workflow_dispatch 时可附带上游同步检查（允许失败继续，仅展示结果）。

## 8. 多租户品牌档案（`--brand`）与配网页定制

一套代码可为多个租户构建各自品牌的固件。租户档案位于 `product/profiles/<tenant>.json`：

```json
{
    "brand_name": "Acme",
    "project_name": "acme-voice",
    "project_ver": "1.0.0",
    "sdkconfig": {
        "CONFIG_PRODUCT_BRAND_AP_PREFIX": "Acme",
        "CONFIG_PRODUCT_BRAND_BLE_NAME": "Acme-Blufi",
        "CONFIG_OTA_URL": "https://ota.acme.com/device/ota/"
    },
    "web": {
        "title": "Acme 设备配网",
        "accent_color": "#e11d48",
        "theme": "",
        "footer": "© Acme",
        "support_url": "https://help.acme.com",
        "logo": "profiles/acme/logo.svg",
        "portal_pack": "profiles/acme/portal",
        "brand_config": {
            "i18n": {"zh-CN": {"wifi_tab": "连接网络"}},
            "sections": {"advanced_tab": true, "tx_power": false},
            "notice": "配网即表示同意《服务条款》"
        }
    }
}
```

- `logo` / `portal_pack` 路径相对 `product/`，且必须留在 `product/` 内。
- `sdkconfig` 值支持字符串/布尔/整数：布尔写 `y`/`n`，字符串自动加引号转义；以 `"` 开头的字符串按原样写入。
- 内置示例：`product/profiles/example.json`。

构建：

```sh
python3 scripts/build.py --list-brands
python3 scripts/build.py esp-box-4 --name acme-esp-box-4 --brand acme
python3 scripts/build.py <board>                  # 不带 --brand = 完全恢复原样
```

构建时 `--brand` 做三件事，全部是生成物（已加入 `.gitignore`）：

1. 品牌项以**最高优先级**注入 sdkconfig（覆盖板级 `config.json`、语言、唤醒词等上层设置）；
2. 把租户资产暂存到 `product/brand_assets/`：`logo`、`brand_config.json`、
   `portal/wifi_configuration{,_done}.html[.gz]`；
3. 生成 `product/brand.gen.cmake`（`BRAND_PROJECT_NAME/VER`），根 `CMakeLists.txt`
   在 `product/brand.cmake` 之后包含它；不带 `--brand` 的构建会清理上述生成物。

### 缓存与 CI

- **staging 是内容稳定的**：只有内容变化的文件才会重写，未变化的文件保持原时间戳，
  因此 CMake 的 `target_add_binary_data` 不会重复嵌入/重链，增量构建直接复用缓存；
  终端会输出 `Brand staging unchanged; reusing previous build cache`。
- 切租户时只清理不再需要的资产（例如新档案没有 logo），不会无差别重写全部文件。
- CI（`.github/workflows/build.yml`）：
  - 变更检测把 `product/` 与 `components/esp-wifi-connect/` 视为全局影响，触发代表性板卡构建；
  - 品牌相关路径变更时额外跑 `brand` 作业：用 `example` 档案构建
    `bread-compact-wifi`，并断言项目名、品牌名与 logo 已进入 `merged-binary.bin`；
  - 矩阵与品牌作业均缓存 `~/.ccache`（按 target/变体分键、按 SHA 滚动），
    代码未变化时复用上一次的编译缓存。

### 配网页定制分档

| 档位 | 交付物 | 效果 |
| --- | --- | --- |
| L1 换肤 | 档案里的 `brand_name`/`web.title`/`accent_color`/`logo`/`footer`/`support_url` | 标题、logo、主题色、页脚、帮助链接 |
| L2 声明式 | `web.brand_config`（`i18n`、`sections`、`theme`、`notice`、`css_vars`） | 全量文案覆盖、模块显隐、公告块、任意 CSS 变量 |
| L3 整页替换 | `web.portal_pack`：`index.html`（必需）、`done.html`（可选） | 完全自定义页面；必须遵守 [配网页契约](../docs/portal-contract.md) |

L3 页面按原文件名嵌入（`wifi_configuration.html` 等），并自动预压缩为 gzip 随固件
下发（`Content-Encoding: gzip`），单页原始上限 512 KB（超过 256 KB 会告警）。
未提供的 `done.html` 自动回退到内置页面。

### 本地开发 L3 页面（无需硬件）

```sh
python3 scripts/product/portal_mock.py --brand acme --port 8000
python3 scripts/product/portal_mock.py --pack product/profiles/acme/portal
```

mock 提供与固件一致的端点（`/scan`、`/submit`、`/saved/*`、`/advanced/*`、
`/done.html`、`/exit`、`/brand.js`、`/brand-logo`）和假网络列表，浏览器直接开发调试；
终端会回显提交事件。契约见 [`docs/portal-contract.md`](../docs/portal-contract.md)。

### 实现位置

- 组件：`components/esp-wifi-connect/`（`78/esp-wifi-connect` 的 OEM fork，见
  `components/README.md`）；提供 `/brand.js`、`/brand-logo` 端点与页面槽位。
- 固件接线：`main/boards/common/wifi_board.cc`（`BuildPortalBrandJson()`）。
- 参数：`main/Kconfig`；资产嵌入：`main/CMakeLists.txt`。
- 管线：`scripts/build.py`（`--brand` / `--list-brands`）；测试：
  `scripts/tests/test_brand_profiles.py`。
