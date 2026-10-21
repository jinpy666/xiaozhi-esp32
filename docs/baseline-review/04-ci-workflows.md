# 基线审查 ④：CI、构建与发布工作流复用与改造清单

> 审查基线：上游 `78/xiaozhi-esp32` 提交 `aab98ab`。只描述现状与缺口，不做设计。

## 1. 仓库与远端现状（方案第八节同步策略的起点）

| 项 | 现状 |
| --- | --- |
| 本地/origin | `https://github.com/jinpy666/xiaozhi-esp32`（唯一 remote） |
| GitHub fork 关系 | `jinpy666/xiaozhi-esp32` **是 `78/xiaozhi-esp32` 的 GitHub fork**（API 确认 `fork: true, parent: 78/xiaozhi-esp32`） |
| 本地分支 | 仅 `main`（与 `origin/main` 一致）；无 `product/dev`、无同步分支 |
| upstream remote | **未配置**（`git remote -v` 仅 origin） |
| 上游默认分支 | `main`（API 确认） |
| 同步基线 | 本地 `main` HEAD = `aab98ab` = 上游 `main` 最新提交（2026-10-19 推送），**落后 0 提交、零定制** |

结论：当前是"刚 fork 的干净上游镜像"，是建立 `product/dev` 与同步流程的最佳时机；首次同步前无需合并冲突处理。

## 2. 工作流清单（`.github/workflows/` 共 3 个）

### 2.1 `build.yml`（Build Boards）

- 触发：`workflow_dispatch`、push 到 `main`/`ci/*`、PR 到 `main`（build.yml:3-12）。权限 `contents: read`。
- Job `prepare`（18-69）：容器 `espressif/idf:release-v6.1`；checkout@v7（fetch-depth 0）；跑 `python -m unittest discover -s scripts/tests -v`；按事件计算变更文件，执行 `python scripts/build.py --select-changed` 输出受影响变体 JSON。
- Job `build`（71-105）：矩阵 = prepare 输出，`fail-fast: false`；构建命令 `python scripts/build.py "$BOARD" --name "$VARIANT" [--build-options-json ...]`；**artifact 仅 `build/merged-binary.bin`**，`if-no-files-found: error`。无发布步骤。
- **矩阵是动态的，非硬编码**：
  - 板卡变体由 `build.py:_collect_variants()` 遍历 157 份 config.json 生成（`scripts/build.py:846-997`）。
  - 变更选择逻辑 `_select_variants_for_changes`（`scripts/build.py:1046-1103`）：公共路径（workflow/CMakeLists/`main/` 非 boards/`main/boards/common/`）变更 → 叠加 `scripts/ci/representative-variants.json` 中人工维护的 **18 个代表性变体**；仅某板目录变更 → 只构建该板全部变体。
  - `workflow_dispatch` 传全量文件 → 代表变体 + 全部受影响变体（build.yml:4 注释"Manual runs exercise the full matrix"）。

### 2.2 `release.yml`（Release Firmware）

- 触发：仅 `workflow_dispatch`，输入布尔 `publish`（默认 true；false 只留 draft）。`release` job 单独提权 `contents: write`（release.yml:100-101）。并发组 `firmware-release`。
- Job `prepare`（20-61）：`publish=true` 时强制默认分支；`python3 scripts/release.py prepare --idf-version 6.1.0 --source-sha <sha> ...` 输出 `version/tag/variants/firmware_count`，上传 `release-manifest.json` artifact。
- Job `build`（63-94）：全量变体矩阵；`python scripts/build.py "$BOARD" --name "$VARIANT" --zip` → `releases/<tag>_<full_name>.zip`；artifact `compression-level: 0`。
- Job `release`（96-135）：下载 manifest + 全部固件（merge-multiple）→ `python3 scripts/release.py publish --manifest ... --assets-dir ... [--make-public]` → 写 GITHUB_STEP_SUMMARY。

### 2.3 `test-mcp.yml`

- 触发：push/PR（paths 过滤：`main/mcp_server.*` 等）+ workflow_dispatch；ubuntu-24.04，apt 安装 g++/libcjson/libmbedtls，跑宿主机 MCP dispatcher 单测。无 artifact、无发布。

## 3. 发布相关脚本

### 3.1 `scripts/release.py`（255 行，已被 release.yml 接入）

- `prepare`（release.py:218-246）：校验 `PROJECT_VER`（当前 `2.5.3`，根 `CMakeLists.txt:12`）为 `x.y.z`、source_sha 为 40 位、IDF ≥ 6.1.0；生成 `release-manifest.json`（含 `version/tag/source_sha/idf_version/board_count/firmware_count/variants`，release.py:80-89）。
- `publish`（release.py:149-215）：
  - Preflight：拒绝覆盖已发布 release 或移动已有 tag（49-65）。
  - `verify_assets`：文件名集合与 manifest 完全一致、ZIP 内只允许 `merged-binary.bin`、解析 app descriptor 校验 version/project/IDF 版本/**chip_id 与 target 匹配**（118-146），生成 `SHA256SUMS`。
  - `gh api releases/generate-notes` 生成 changelog（含 source SHA 与 run URL 元数据）；`gh release create --draft`；分批上传 zip+manifest+SHA256SUMS；回读校验远端资产集合/大小/digest；`--make-public` 时转正式。

### 3.2 `scripts/versions.py`（OTA 通道，**未接入任何 workflow**）

从 `releases/` 解包 merged bin、读 app descriptor、上传阿里云 OSS（oss2）并向版本服务器 POST info.json（versions.py:168-245）。属人工/上游使用的手动脚本，全 workflow 目录 grep 无引用。

## 4. 依赖锁定现状

| 项 | 现状 | 位置 |
| --- | --- | --- |
| Actions 版本 | 均为**浮动 major tag，未 pin commit SHA**：`actions/checkout@v7`（6 处）、`actions/upload-artifact@v7`（3 处）、`actions/download-artifact@v8`（2 处）；全部第一方，无第三方 action | build.yml:27,84,101；release.yml:30,56,74,88,103,112；test-mcp.yml:27 |
| ESP-IDF 版本 | 容器镜像 `espressif/idf:release-v6.1`（**浮动 release 分支 tag**）；release 清单固定传 `--idf-version 6.1.0`，但校验只比对 major.minor | build.yml:22,81；release.yml:68,51；release.py:129,140-142 |
| 组件版本 | `build.py:1064` 的变更路径清单引用 `dependencies.lock`，但**该文件在仓库中不存在**（已验证）——组件版本未锁定 | — |
| Dependabot | `.github/` 下无 dependabot 配置 | — |

## 5. 六项能力对照（现状缺口）

| 能力 | 现状 |
| --- | --- |
| 多型号产品构建矩阵 | **已有**：动态矩阵（191 builds）、代表变体机制、IDF 版本门控、`--select-changed` 按变更裁剪 |
| 固件归档 | **部分**：CI artifact 仅有单 bin（临时，未设 retention）；GitHub Release 资产（zip+SHA256SUMS+manifest）为持久归档；无 Release 之外的长期归档 |
| 构建元数据 | **部分**：release manifest 含 source_sha/idf_version/version/变体列表；**缺失**：CI artifact 无任何元数据、manifest 无逐固件哈希、无容器镜像 digest/工具链指纹 |
| 签名 | **缺失**：三工作流均无签名步骤；`sdkconfig.defaults*` 无任何 `CONFIG_SECURE_*`（secure boot）；release.py 仅 descriptor/哈希校验 |
| 灰度发布 | **缺失**：仅 draft/公开两态；无 beta 通道、按设备/比例推送；versions.py 单通道且未接入 CI |
| 设备回归 | **缺失**：仅宿主测试（Python 工具单测 + MCP 单测）；无硬件在环 job；release.py 发布说明也明确"不替代真机验证" |

其他：所有工作流无 schedule 触发；发布完全依赖人工 `workflow_dispatch`，无 tag 触发。

## 6. 复用 / 改造清单

| 现有资产 | 复用方式 | 需要的改造 |
| --- | --- | --- |
| `build.yml` 动态矩阵 + `--select-changed` | 直接复用；变更裁剪机制已能控制 CI 时长 | 增加"按产品 SKU 过滤矩阵"的输入（现状矩阵是上游全量 191 builds） |
| `scripts/ci/representative-variants.json` | 机制直接复用 | 内容改为覆盖**自有产品 SKU** 的代表变体（而非上游全芯片代表） |
| `scripts/release.py` | 直接复用（防覆盖、chip_id 校验、SHA256SUMS、manifest、draft 流程） | manifest 增加逐固件哈希/产品 SKU/功能开关字段；签名可在 publish 前插入 |
| `scripts/tests/`（10 个宿主测试，PR 必跑） | 直接复用 | 新增产品配置 schema/组合校验测试 |
| `scripts/build.py` 的解析/合并机制 | 作为产品配置层的承载点（见审查 ① §7） | 增加产品 profile 合并层 |
| `scripts/versions.py` | 可复用为自有 OTA 发布通道 | 目前单通道、无灰度、未接 CI；接入前需设计 |
| `build.yml` 的 `espressif/idf:release-v6.1` | 现状即用 | 如需可复现构建：固定补丁版本 + 锁 `dependencies.lock` |
| Actions 版本 | — | 正式发布工作流建议 pin commit SHA（现状浮动 tag） |

## 7. 待验证项

1. `workflow_dispatch` 时矩阵是否严格等于全量，是基于 `build.py:1046-1103` 逻辑的推读，未实际执行 `--select-changed` 验证（与 build.yml:4 注释一致）。
2. `scripts/ci/blufi.sdkconfig.defaults` 的具体内容与引用关系未核实（三个 workflow 中未出现该文件名，应由 sdkconfig_append 引用）。
3. `versions.py` 是否被 docs/ 或外部流程调用未全面排查。
4. Actions `@v7`/`@v8` major tag 的实际存在性与行为以 GitHub 官方为准（报告如实摘录仓库写法）。
