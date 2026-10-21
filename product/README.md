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

`main/Kconfig` 定义两个可覆盖的品牌符号（均有上游默认值）：

| Kconfig 符号 | 用途 | 默认值 |
| --- | --- | --- |
| `CONFIG_PRODUCT_BRAND_AP_PREFIX` | Wi-Fi 配网 AP SSID 前缀 | `Xiaozhi` |
| `CONFIG_PRODUCT_BRAND_BLE_NAME` | BluFi 蓝牙设备名 | `Xiaozhi-Blufi` |

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
