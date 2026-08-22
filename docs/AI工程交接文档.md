# 原版氪奇精灵扩展：AI 工程交接文档

> 文档状态：2026-08-22，与当前工作区源码同步。  
> 工程目录：`D:\奥奇工程\原版氪奇精灵扩展`  
> 部署目录：`D:\奥奇工程\氪奇Pro-V1.1.3`  
> 官方解包参考：`D:\奥奇工程\奥奇传说解包`  
> 逆向证据参考：`D:\奥奇工程\逆向\逆向`

## 0. 给接手 AI 的第一段话

这是一个“外置启动器 + 注入式 Qt DLL”，功能运行在**原版氪奇登录器**进程内，不是复刻登录器。原版没有工程源码，所以 `KQPetLauncher.exe` 启动目录中版本最高的 `KQPro*.exe`，再通过 `LoadLibraryW` 注入 `KQPetInventory.dll`。DLL 复用原版协议发送函数、只读捕获 `recivedata`，并创建三个独立 Qt 大窗口：精灵背包/仓库、兑换商店、日常活动。

继续工作前按顺序阅读：

1. 本文档。
2. `CMakeLists.txt`。
3. `src/extension/extension_context.cpp`。
4. `src/extension/original_bridge.cpp`。
5. `src/extension/pet_repository.cpp`。
6. 对应功能的 `*_controller.cpp`、`*_window.cpp`。
7. 同名的 `tests/*_smoke.cpp` 或 `tests/*_ui_preview.cpp`。

不要从 `D:\奥奇工程\逆向\逆向` 开始改功能；它只是证据和历史复刻工程。

## 1. 绝对边界

1. 不允许修改、替换、打补丁或重新签名任何 `KQPro*.exe`。
2. 当前核对目标 `KQProV1.1.3.exe` 的 SHA-256 是：

   `7EEA6EF8806DD74F8E17FFC062339616C7CEE51C50F94F140AA74A73E2F30A3B`

3. 只部署 `KQPetLauncher.exe` 和 `KQPetInventory.dll`。运行中 DLL 被锁定时，部署脚本写成 `KQPetInventory.pending.dll`，下次启动器运行前再替换。
4. 所有网络等待、批量队列和图片下载都必须异步；禁止在 Qt 主线程 `sleep`。
5. 精灵个体唯一键永远是“账号 ID + 协议实例字段 `id`”。`r/ri` 是种族/形态，`fr` 是形象，名称和位置都不是实例键。
6. 所有服务器响应写盘前必须核对账号、会话代数、请求代数和实例 ID。账号切换后的旧响应只能丢弃。
7. 详情写入使用 `QSaveFile` 原子替换；失败、超时、空对象和残缺对象不得覆盖旧缓存。
8. 不把 `KQPetData/accounts`、账号 ID、精灵数据、商店次数或任务进度提交到 Git/发行版。

## 2. 总体架构

```text
KQPetLauncher.exe
  ├─ 找到同目录版本最高的 KQPro*.exe
  ├─ 激活 pending DLL（如果存在）
  ├─ CreateProcessW 启动原版
  └─ CreateRemoteThread + LoadLibraryW 注入 KQPetInventory.dll

KQPetInventory.dll
  └─ ExtensionContext
      ├─ OriginalBridge              原版协议发送/消息捕获
      ├─ PetRepository               账号隔离内存模型与磁盘缓存
      ├─ PetRefreshController        列表、详情、移动状态机
      ├─ ShopExchangeController      商店次数与货币查询
      ├─ RoutineOverviewController   日常/周常、活动红点、玩法次数查询
      ├─ PetWindow                   背包/普通仓库/精英仓库
      ├─ ShopWindow                  六个兑换商店
      └─ RoutineOverviewWindow       日常、周常、活动与玩法次数概要
```

`ExtensionContext` 是总装配点：创建模型和控制器，把 `OriginalBridge::packetReceived` 分发给 Repository、商店控制器和任务控制器，并把按钮挂到原版主窗口。新增跨功能依赖优先在这里连接，不要让窗口直接调用桥接底层。

## 3. 原版桥接

核心文件：

- `src/extension/original_bridge.cpp/.h`
- `src/extension/inline_hook.cpp/.h`
- `src/loader/main.cpp`

已知 V1.1.3 RVA：

| 功能 | RVA |
|---|---:|
| WebViewService 单例取得 | `0x115450` |
| `recivedata/cutdata/otherreturn` 分发 | `0x115920` |
| Flash 命令发送 | `0x116470` |

更新版优先尝试唯一签名扫描。三个入口不能唯一定位时必须安全停止扩展，不能沿用旧 RVA 猜测调用。

发送线格式：

```text
Extension|command|json
```

桥接最终复用原版执行：

```javascript
document.myFlash.senddata(ext, cmd, JSON.parse(params), 'xml', -1)
```

`OriginalBridge::send()` 用于正常协议命令；`invokeFlash()` 只用于已有证据证明必须直接调用 Flash 方法的场景。

## 4. 当前协议表

| 功能 | Extension | Command | 参数 | 返回要点 |
|---|---|---|---|---|
| 登录账号识别 | 原版消息 | `21_1` | — | `info.n` 作为账号分区键 |
| 背包列表 | `PJXExtension` | `2_1_10` | `{}` | `pl`、`pps`、`ppc` |
| 仓库摘要 | `PJXExtension` | `2_1_S` | `null` | `ns` 普通、`es` 精英、`rb` 告别 |
| 单只详情 | `PJXExtension` | `2_1_R` | `{"pi":实例ID}` | `p` 完整个体 |
| 写背包序列 | `PJXExtension` | `2_1_11` | `pps/ppt` | 移动或替换后序列 |
| 仓库变化通知 | `PJXExtension` | `2_1_25` | 推送 | 触发只读核对 |
| 阵型载入/变化 | `PJXExtension` | `2_2_10` 等 | 依协议 | 背包“是否出阵” |
| 兑换次数 | `TimelinessActExtension` | `1008_20260313_es_0` | `{}` | `si* / bi*` |
| 账号材料 | `MaterialExtension` | `3_11` | `{}` | 商店货币数量 |
| 日常/周常 | `TimelinessActExtension` | `1008_20170623_dt_0` | `null` | `ti/wti/av/wav/bi/wbi` |
| 活动红点 | `null` | `1037_0` | `{"ids":"lights"}` | `rs` 中 `#` 分隔红点 ID |
| 星轮玩法次数 | `TimelinessActExtension` | `1008_20220603_swa_0_0` | `null` | `ti` 今日剩余，`wgt` 本周已进行 |

商店命令和项目定义不要硬编码到窗口；以 `assets/shop-exchange-data.json` 和用户缓存中的新版 catalog 为准。

## 5. 数据模型和缓存

默认数据根目录是启动器/原版目录下 `KQPetData`。测试可通过 `KQPET_DATA_ROOT` 覆盖。

```text
KQPetData/
├─ last-account.txt
├─ settings.json
├─ accounts/<安全账号键>/
│  ├─ inventory.json
│  ├─ details/<实例ID>.json
│  ├─ shops.json
│  └─ routines.json
├─ catalog/
│  ├─ pet-detail-data.json          可选外部新版字典
│  ├─ shop-exchange-data.json       动态商店目录
│  └─ routine-overview.json         动态任务/活动目录
└─ images/
   ├─ attributes/
   └─ pets/
```

账号键若不是安全 ASCII，会先 SHA-256 截断，防止目录穿越和非法文件名。

详情 schema 3 的核心字段：

```json
{
  "schema": 3,
  "account": "账号键",
  "instanceId": 1192,
  "raceIdAtSave": 7152,
  "faceIdAtSave": 123,
  "displayNameAtSave": "皮肤显示名",
  "obtainedAt": "gd",
  "savedAt": "ISO-8601",
  "imageCacheKey": "race_face_hash",
  "pet": {}
}
```

仓库概览战斗力只能读当前账号、当前实例 ID 的详情文件。仓库摘要里的战力字段不能作为替代。详情目录不做 30 天清理；个体离开仓库后文件保留。

## 6. 精灵刷新和移动状态机

核心文件：`pet_refresh_controller.cpp/.h`。

默认时序：

- 自动列表：60 秒。
- 背包与仓库请求间隔：1 秒。
- 列表单项超时：10 秒。
- 仓库详情：单并发、每只 1 秒、每批 12 只、批间 2 秒、单只 8 秒、失败重试 1 次。
- 手动详情批量运行或暂停期间停止自动列表倒计时；结束后重新完整计时。
- 点击单只仓库精灵进入最高优先级队列，但不打断当前已发送请求。

移动流程不是乐观改 UI：

```text
等待其他刷新空闲
  → 强制刷新列表做 preflight
  → 校验实例和背包容量
  → 背包满时要求用户明确选择替换对象
  → 只发送一次 2_1_11
  → 不重试写请求
  → 再次只读刷新列表
  → 以服务器最终列表判断成功/失败
```

移动后不要刷新商店兑换次数或官方 catalog；只刷新背包/仓库及个体状态，避免无关请求冲突。

## 7. 精灵显示规则

核心文件：

- `pet_window.cpp/.h`
- `pet_detail_catalog.cpp/.h`
- `pet_search.cpp/.h`
- `pet_image_cache.cpp/.h`

重要约定：

- 背包按官方 `_position` 默认排序，每页 12 只，两排，每排 6 只。
- 普通/精英仓库共享仓库排序，但和背包排序独立。
- 搜索支持中文连续子串、皮肤名、原名、实例 ID 和连续拼音首字母。
- 搜索命中字符显示红色粗体。
- 双职业筛选任一职业均可命中。
- 详情关系包括召唤、被召唤、携带、被携带、神使。
- 红星、金星、万变底座、星轮突破和最高战力规则都集中在详情生成逻辑，修改时先扩充 `catalog_smoke` 或 `repository_smoke`。

静态元数据来自 `assets/pet-detail-data.json`，由 `tools/generate_pet_detail_data.py` 从官方解包生成。未来新精灵/皮肤优先更新外部 `KQPetData/catalog/pet-detail-data.json`，不必立即重编译 DLL。

## 8. 兑换商店

核心文件：

- `shop_exchange_catalog.cpp/.h`
- `shop_exchange_controller.cpp/.h`
- `shop_pet_eligibility.cpp/.h`
- `shop_window.cpp/.h`
- `tools/generate_shop_exchange_data.py`

设计：

- “刷新兑换次数/货币”只查账号状态。
- “更新兑换项目/适用精灵”扫描官方 `SEFConfig.as`，更新项目名、资源、限次、允许种族和培养类型。
- 稳定身份使用商店 ID、服务器项目 ID、强化类型等，不使用中文描述作为主键。
- 适用性绿色高亮是客户端分析提示，最终能否兑换永远由服务器决定。
- 点击精灵先显示本地详情，再对相同账号/实例 ID 请求一次 `2_1_R`。
- 仓库精灵可进入背包；背包精灵不显示入库按钮。
- 当前选中兑换项目的名称字体加粗，切换项目时取消旧项目粗体。

## 9. 日常、周常和活动概要

核心文件：

- `routine_overview_catalog.cpp/.h`
- `routine_overview_controller.cpp/.h`
- `routine_overview_window.cpp/.h`

只有手动刷新，没有后台自动查询。

日常和周常的“距任务完成”是：

```text
max(0, 配置完成目标 - 服务器当前进度)
```

这是任务目标差值，不是“农场今日还能玩 8 次”这类玩法机会。窗口另有“玩法剩余次数”页，只接入已确认官方查询字段的活动：

- 星轮：`TimelinessActExtension / 1008_20220603_swa_0_0 / null`；`ti` 是今日剩余，`wgt` 是本周已进行，官方配置上限为每日 3、每周 6。
- 竞技场：`null / 16_24_A / null`；`sweep` 是今日已扫荡次数，`zao1/zao2.ct` 是两场今日已挑战次数，`bct` 是已购买次数；官方每日扫荡、金币奖励和分场基础挑战上限均为 8。
- 精灵公园 6 合 1：`PetParkExtension / 100_13_0 / null`；`pt` 是本周已使用次数，官方周上限为 3。
- 精灵公园新手带回：`PetParkExtension / 100_2_0 / {"m":"账号"}`；`rfc` 是服务器直接返回的剩余次数。当前资源未确认统一上限，因此界面只显示剩余值，不伪造上限。

当前官方 `ServiceConfig` 中的旧 `FarmService` 已标记为 `____DISABLED___`，所以不得自行猜测农场命令。如果用户指的是某个当前活动的“农场”，先根据它的准确游戏名称找到具体 model/service 回包，再增加独立 adapter。

活动没有统一的“剩余挑战次数”响应，因此活动页显示的是当前点亮的官方红点节点数量，并明确标为“剩余待处理项（红点）”。不能把无红点解释成整个活动已经完成。

手动刷新中的日常/周常、活动红点和各玩法次数是独立子请求。任意一个被拒绝、超时或数据不完整时，只保留该部分旧缓存，其他有效回包继续保存。玩法回包以命令为键保存到 schema 3 的 `opportunityPackets`，旧 schema 2 星轮缓存会自动迁移读取。

动态活动目录来自官方 HUD/红点配置。刷新成功后整表替换，因此新增活动自动出现，官方已删除活动自动消失。

## 10. 构建、测试、预览和部署

依赖：

- Windows x64。
- Qt 6.6.3 MSVC 2019 64-bit：`D:\QT\6.6.3\msvc2019_64`。
- Visual Studio C++ x64 工具链。
- CMake 3.24+ 与 Ninja。

构建：

```powershell
Set-Location 'D:\奥奇工程\原版氪奇精灵扩展'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
  -QtRoot 'D:\QT\6.6.3\msvc2019_64' `
  -BuildDirectory 'build-codex-current'
```

不要直接在未初始化 MSVC 环境的普通 PowerShell 中调用 `cmake --build`；可能报标准库头文件 `limits` 不存在。`build.ps1` 会通过 `VsDevCmd.bat` 导入环境。

测试：

```powershell
ctest --test-dir build-codex-current -C Release --output-on-failure
```

界面自检：

```powershell
$env:Path='D:\QT\6.6.3\msvc2019_64\bin;' + $env:Path
$env:KQPET_PREVIEW_SELF_TEST='1'
.\build-codex-current\bin\Release\KQPetUiPreview.exe
.\build-codex-current\bin\Release\KQShopUiPreview.exe
.\build-codex-current\bin\Release\KQRoutineUiPreview.exe
```

部署：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy.ps1 `
  -OriginalDir 'D:\奥奇工程\氪奇Pro-V1.1.3' `
  -BuildBin 'D:\奥奇工程\原版氪奇精灵扩展\build-codex-current\bin\Release'
```

部署后必须核对：

1. 构建产物与部署文件 SHA-256 相同。
2. 原版 EXE SHA-256 未变化。
3. 没有意外复制测试 EXE 或账号缓存到源码/发行包。

## 11. 自动测试覆盖

| 测试 | 主要覆盖 |
|---|---|
| `catalog_smoke` | 精灵静态字典、详情名称与分类 |
| `repository_smoke` | 账号隔离、实例隔离、详情 schema、旧响应拒绝 |
| `refresh_controller_smoke` | 60 秒列表、1 秒间隔、详情单并发、暂停/继续/取消、优先插队 |
| `move_controller_smoke` | 入库/进背包、满包替换、写后只读核对 |
| `search_smoke` | 中文/拼音连续搜索 |
| `shop_exchange_smoke` | 商店动态目录、次数、资格分析、账号隔离 |
| `routine_overview_smoke` | 六个手动请求、任务/活动/玩法缓存、部分失败与账号切换拒绝 |
| 三个 UI preview | 布局、排序、粗体选中、玩法剩余次数 |

修改状态机或缓存规则时，先写失败测试再改实现。窗口测试使用匿名模拟数据，不连接账号。

## 12. 环境变量

| 变量 | 用途 |
|---|---|
| `KQPET_DATA_ROOT` | 覆盖缓存根目录，主要用于测试 |
| `KQPET_CATALOG_PATH` | 指定外部精灵元数据字典 |
| `KQPET_OFFICIAL_UNPACK_ROOT` | 指定官方解包扫描根目录 |
| `KQPET_PREVIEW_SELF_TEST` | UI preview 自动断言后退出 |
| `KQPET_PREVIEW_EXIT_MS` | UI preview 延时退出 |

## 13. 常见故障定位

### 启动器点击没反应

1. 确认三个文件同目录：`KQPetLauncher.exe`、`KQPetInventory.dll`、`KQPro*.exe`。
2. 检查是否有 `KQPetInventory.pending.dll` 和被占用旧进程。
3. 检查 Windows 安全软件是否拦截 `CreateRemoteThread`。
4. 直接运行原版能否启动；若原版本身失败，先排除扩展。

### 更新版氪奇打不开扩展

查看 `OriginalBridge::lastError()`。最常见原因是入口签名改变或签名不再唯一。重新核对新 EXE 的三个函数并更新签名，不要盲改 RVA。

### 仓库数据串账号

检查 `PetRepository::activateAccountSession()`、所有 expectation 和 controller 的 `requestAccount_/requestSessionGeneration_`。禁止绕过会话代数直接写盘。

### 点击详情卡顿

只更新实例对应行。非选中实例不要生成详情 HTML、原始树或加载大图；批量响应不要重建整个表格。

## 14. 发布与隐私检查

提交或打发行包前执行：

```powershell
git status --short
rg -n "accounts|instanceId|账号|KQPetData" .
```

人工确认：

- 不包含 `KQPetData/accounts`。
- 不包含真实 `inventory.json/details/*.json/shops.json/routines.json`。
- 不包含真实账号截图和日志。
- 发行版只包含编译后的启动器、DLL、说明和必要许可文件。
- README 保留“如有侵权，请联系删除”。

## 15. 接手 AI 完成一次改动的标准流程

1. 读本交接文档和目标模块测试。
2. 用 `git status --short` 识别用户已有改动，不覆盖无关文件。
3. 明确协议事实、缓存事实和 UI 展示事实，不用字段名猜写操作。
4. 先补测试夹具；任何真实账号样本都必须匿名化后才能进入 `tests`。
5. 使用 `apply_patch` 修改源码。
6. 用 `scripts/build.ps1` 构建。
7. 运行完整 CTest 与受影响 UI preview。
8. 部署前后核对原版 EXE 哈希。
9. 最终说明修改文件、缓存影响、测试结果、部署结果和已知边界。

## 16. 当前建议的后续方向

1. 给特定活动增加独立协议 adapter，从而显示真正的活动剩余挑战/购买次数；保持通用红点页作为兜底。
2. 每次原版氪奇升级后先做只读签名兼容测试，再允许发送协议或移动精灵。

本文档是当前工程的权威交接入口；旧的 `设计与逆向依据.md` 保留历史逆向证据，但其中部分“当前限制”和早期缓存说明已过时，以本文档和当前源码为准。
