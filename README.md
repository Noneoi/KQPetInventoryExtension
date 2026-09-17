# 氪奇精灵工作台

氪奇 Pro 的非官方 Windows 扩展，提供精灵列表与详情、培养分析、指定精灵兑换查询、日常活动和账号资产汇总。通过外置启动器与进程内 Qt DLL 工作，原版客户端 EXE 保持原样。

当前仓库为 **2.0.0 源码**。已有开发构建通过完整 Release 回归（72 项）；真实客户端交互由用户手动验证。二进制的具体版本、源码摘要和测试结果以对应产物的 identity 与报告为准。

## 安装与升级（推荐）

在 [v2.0.0-preview.2 发行页](https://github.com/Noneoi/KQPetInventoryExtension/releases/tag/v2.0.0-preview.2) 下载名称含 **copy-ready** 的复制即用 ZIP。

1. 关闭正在运行的氪奇客户端。
2. 解压下载的压缩包。
3. 将 **启动精灵工作台.cmd** 和 **KQPetQuickStart** 文件夹一起复制到 `KQPro*.exe` 所在目录。
4. 双击 **启动精灵工作台.cmd**。

```text
氪奇客户端目录/
├─ KQPro*.exe
├─ 启动精灵工作台.cmd
└─ KQPetQuickStart/
```

程序自动安装或更新扩展并启动，无需手工填写路径或输入命令。原版客户端和已有缓存保留。以后升级同样先关闭氪奇，用新包中的两项启动文件替换旧文件，再双击启动。

首次使用可在设置点击“检查数据更新”；商店中的“刷新兑换次数”用于手动读取账号状态。

## 主要功能

- **精灵管理**：显示名称与精灵原名分列，支持搜索、筛选、排序、背包分页和仓库按时代刷新。详情刷新期间保留当前内容，避免列表更新后详情停在加载状态。
- **精灵分析**：区分官方极限战斗力、已装备战力、本地持有可达战力与至高战力。未装备但已在本精灵星神背包中的红星参与合法配置计算，区分缺少、待装备及需要升级。
- **培养缺口**：列出元魂等级与觉醒、源兽星级和阶级、星轮、普通红星及万变红星需求；对应元魂、源兽和精华显示“需要 / 已有 / 还差”。
- **官方时代标记**：优先使用种族定义中的 `sign`，皮肤名称不会改变时代判断。精灵详细只显示该时代有的养成（如神职看潜能和源兽装备，启元看元魂但不看星轮/神源兽）。只有灵初精灵计入星轮突破及突破后星灵配置。
- **至高后缀**：列表仅在本地持有可达战力与至高目标均已知，且前者达到目标时显示“（至高）”。官方极限值不作为这条判定线；已有但未装的资源仍可能需要调整装备。
- **资产与红星统计**：全账号养成分析手动计算；“统计本地红星”独立统计本地详情中的已装备及本宠背包，普通红星、万变红星分别计数，不联网。
- **商店切换**：复用已准备的精灵事实与商品规则，后台分批匹配。切换商店或商品不再同步重算全仓库，也不自动查询次数或材料余额。

## 刷新与本地缓存

| 操作 | 行为 |
| --- | --- |
| 启动、切页、排序、筛选、翻页 | 使用已有本地数据 |
| 点击精灵 | 先显示本地详情；游戏在线时查询这一只，成功后更新并保存 |
| 刷新背包/仓库 | 更新精灵列表 |
| 刷新仓库详情 | 查询勾选时代的精灵，支持暂停、继续、取消 |
| 精灵分析 → 刷新材料背包 | 手动读取普通材料与空闲源兽仓库 |
| 商店 → 刷新兑换次数 | 手动读取主商店和已支持活动的次数、货币与价格条件 |
| 设置 → 检查数据更新 | 联网更新官方公共资料、兑换目录和图片资源定义 |
| 查看图片 | 已有有效图片只读本地；缺图允许单独下载并保存 |

详情和图片长期保存，同一实例或资源的最新有效数据覆盖原文件，不按每次刷新堆积版本。断网、超时或无效响应保留旧数据及时间。程序不定时查询仓库、检查公共更新或清理这些磁盘缓存。

默认数据目录是客户端下的 `KQPetData`，独立于插件版本目录。设置可查看占用、打开目录、管理缓存、备份恢复和迁移；`KQPetDataRoot.json` 保存目录选择，迁移在下次启动生效。

```text
KQPetData/
├─ accounts/<账号>/
│  ├─ inventory.json
│  ├─ details/<实例ID>.json
│  ├─ derived/
│  ├─ cultivation-materials.json
│  ├─ activity-exchanges.json
│  └─ shops.json / routines.json / asset-analysis.json
├─ catalog/
├─ images/pets/                     种族与外观 ID 对应的图片
├─ images/stargods/                 星神图标
├─ images/attributes/               属性图标
└─ data-tools/                      手动更新使用的本地转换工具
```

“刷新材料背包”合计空闲源兽各等级的堆叠数量，不把已装备源兽计作可消耗库存。只读观察也能保存到本地，保存成功不会授予游戏写操作权限。

## 官方资料与活动兑换

公共更新覆盖精灵字典、属性与职业名称、星神规则、元魂材料、动态源兽计划、星轮节点与精华、指定精灵兑换、日常活动及图片索引。新活动从官方注册表、HUD、近期发布记录及显式链接发现；相同结构可自动解析，未支持的新结构保留旧数据并报告原因。

2026-09-13 的公开资源快照包含 **8 个活动目录、72 项指定精灵兑换**：

- 72 项有静态限次额度，69 项费用固定，3 项按活动总购买次数选择价格档位。
- 7 个目录的 68 项支持通过“刷新兑换次数”读取账号次数及活动币；同次刷新也读取标准货币与道具余额。
- 圣冕秘阁 4 项已显示费用和总限 1 次，当前次数读取仍缺少宿主生成的 `ci` 参数接入。

活动币及观察按账号、活动来源和查询范围保存，不因同名或同编号而混用。界面区分“只读观察”和“上次”数据，读取成功不自动证明限次周期仍有效。上述数量是当前发现范围的快照，不代表全部历史活动或未来所有规则。

首次点击“检查数据更新”会准备便携 Python、Java 和 JPEXS 工具，可能需要几分钟；后续复用本地工具与同版本资源。公共资料更新不会代替账号查询。

## 构建

需要 Windows x64、MSVC C++ 工具链、CMake 3.24+，以及 **Qt 6.6.3 MSVC 2019 64-bit** 开发包。构建固定使用该 Qt 版本；扩展与宿主共享 Qt ABI，运行时必须通过架构、导出符号及客户端接口检查。

```powershell
git clone https://github.com/Noneoi/KQPetInventoryExtension.git
Set-Location .\KQPetInventoryExtension

# 将 QtRoot 改为自己的 Qt 安装目录。
.\scripts\build.ps1 -QtRoot 'D:\Qt\6.6.3\msvc2019_64'
ctest --test-dir .\build-v2 -C Release --output-on-failure
```

默认构建目录是 `build-v2`，配置为 `Release`；可指定 `-BuildDirectory`、`-Configuration` 和 `-Targets`。主要产物位于 `build-v2\bin\Release`：

| 产物 | 用途 |
| --- | --- |
| `KQPetInventory.dll` | 进程内扩展 |
| `KQPetLauncher.exe` | 配对版本加载器，由稳定入口调用 |
| `KQPetBootstrap.exe` | 打包后作为客户端根目录的 `KQPetLauncher.exe` |
| `KQPetReleaseCheck.exe` | 读取构建身份、检查发行文件 |
| `KQPetCompatibilityCheck.exe` | 离线兼容检查 |
| `KQWorkbenchUiPreview.exe` | 使用模拟数据的独立工作台预览 |

客户端适配不限定氪奇 1.1.3/1.1.4，也不依赖固定文件名、整文件哈希或历史地址。启动器与 DLL 检查当前客户端接口；未识别或 ABI 不兼容时给出原因。通过检测不等于所有未来客户端都保证可用。详见 [客户端版本自适配](docs/compatibility-adaptive.md)。

## 开发打包与高级部署

预览包使用同一构建的加载器、DLL 和测试报告，避免混用旧产物。以下发行脚本使用 `build.ps1` 中的默认 Qt 路径，按本机环境配置后运行：

```powershell
$reportDir = Join-Path (Get-Location) ('build-v2\validation\preview-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
.\scripts\test-release.ps1 -BuildDirectory .\build-v2 -ReportDirectory $reportDir
.\scripts\package-release.ps1 -TestReport (Join-Path $reportDir 'test-report.json') -Preview
```

报告绑定具体源码和产物。`test-release.ps1` 运行完整注册测试；平时也可针对改动运行相关测试。正式包额外要求对应的 `-AcceptanceReport`，定向测试记录不能代替真实客户端验收。

### 生成复制即用包

将已验证标准包的解压目录传给包装脚本：

```powershell
.\scripts\package-copy-ready.ps1 -PackageDirectory '.\dist\已验证标准包目录'
```

复制即用包增加自动部署入口，沿用标准包中已验证的应用二进制、manifest 和测试报告，不重新编译或改变应用版本。

### 高级：手动部署标准包

需要手动指定目录时，关闭测试客户端后执行：

```powershell
$client = 'C:\Games\KQPro-test'
$package = 'C:\Downloads\实际预览包目录'
& "$package\tools\deploy.ps1" -OriginalDir $client -PackageDirectory $package -AllowPreview
& "$client\KQPetLauncher.exe"
```

启动客户端根目录的稳定入口，不直接运行构建目录中的版本加载器。配对版本位于 `KQPetRuntime\releases\<releaseId>`；运行中的客户端收到更新时只暂存，关闭后重新部署完成激活。发行包不包含原版客户端或个人数据。

关闭客户端后，可用同一包内的 `rollback.ps1 -OriginalDir $client` 回退，或用 `uninstall.ps1 -OriginalDir $client` 卸载扩展入口。个人数据与原版客户端保留。

## 文档

| 内容 | 文档 |
| --- | --- |
| 版本变化 | [发行说明](RELEASE_NOTES.md) |
| 数据目录、缓存、备份与迁移 | [本地缓存与手动更新](docs/local-cache-and-manual-updates.md) |
| 官方资料与图片更新 | [手动公共数据更新](docs/manual-public-data-updater.md) |
| 战力组成、星神与至高判定 | [战斗力分析](docs/pet-power-composition.md) |
| 单只分析与资产汇总何时更新 | [分析刷新机制](docs/pet-analysis-lifecycle.md) |
| 培养材料及空闲源兽 | [材料规则](docs/cultivation-material-rules.md)、[源兽仓库读取](docs/source-beast-inventory-protocol.md) |
| 活动自动发现与次数、费用来源 | [活动兑换目录](docs/activity-exchange-public-data.md)、[只读观察映射](docs/activity-exchange-observations.md) |
| 版本适配与逆向依据 | [兼容检测](docs/compatibility-adaptive.md)、[设计依据](docs/设计与逆向依据.md) |
| 开发维护 | [工程交接](docs/AI工程交接文档.md)、[重构实施记录](docs/v2.0-implementation-log.md) |

本项目与奥奇传说、氪奇官方无隶属关系。
