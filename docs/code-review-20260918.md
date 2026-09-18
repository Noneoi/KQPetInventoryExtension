# 代码评审与修复记录 2026-09-18

对应预览构建 `2.0.0-421e66b1beef-20260918T122129Z`，完整 Release 回归 72/72 通过。

本次是一轮全量通审：先出分级清单，再逐项修复。记录同时保留了两处**误判**及其暴露方式，因为它们说明了这个仓库里哪几类改动最容易出错。

## 一、精灵详细界面：契约与召唤关系（功能）

### 只在确有关系时显示

此前无论精灵有没有神使、召唤物或契约关系，详细界面都会渲染这些区块。现在由 `DetailPage::relationsApplicable` 决定：只有官方字段里真的存在关系值或可契约候选时才出现对应区块，区块名也拆成「召唤物关系」和「神使契约」。

配套修复了一个真 bug：关系解析只把 `0` 当作「无关系」，而官方协议用 `-1` 作为哨兵值，导致几乎每只精灵都会渲染出一行「实例 ID 无效」。现在由 `absentReference()` 统一判定，`<= 0` 一律视为无关系。

### 只显示精灵实际所处的那一侧

一只精灵要么是携带者（能契约别人），要么是被契约的神使，两者不会同时成立。`DetailPage` 新增 `contractCarrierRole` / `contractEnvoyRole` 两个角色标记，界面据此只渲染它实际所处的一侧，不再把「已契约」和「被契约」两块都摆出来。

### 可契约候选的排版

候选数量多时旧版会把整个界面撑乱。现在折叠收纳 + 4 列网格 + 显示总数，`carryCandidateCount` 单独携带计数，避免为了显示数量而展开全部条目。

### 关系条目本身

每个关系条目现在显示：精灵名、原名、所在位置、等级、当前战力、极限战力。位置由 `DetailOwnership` 枚举给出（背包 / 普通仓库 / 精英仓库 / 告别仓库 / 未持有），账号里没有的精灵单独归为一组。条目可点击，弹出该精灵的完整详细界面——弹窗使用独立的详情订阅槽位（consumer 2，与「当前选中精灵」的 0 和「商店」的 1 并列），`kDetailConsumerCount` 相应增加到 3。

## 二、分级修复清单

### P0：详情订阅被静默丢弃

`InventoryPublisher::setDetailInterests` 里仍写死 `if (ids.size() > 2) return;`，而详情槽位已因关联精灵弹窗扩到 3 个。结果是三个槽位同时订阅时，**全部**订阅被丢弃且不报错。改为对 `kDetailConsumerCount` 判定，并补上断言说明这个上限的含义。

这是上一轮自己引入的回归，靠通审才发现——常量改了、判定没跟着改，编译器不会提醒。

### P1

- **天赋槽位**：纪元无法解析时，代码会按猜测的布局选 8 条或 6 条通道，把另外几条真实属性直接藏掉。改为纪元未知时展示全部 12 条（`PetEraSystems::allTalentLanes`），宁可多显示也不静默丢数据。
- **表格行复用残留**：`fillRow` 在源行不存在时直接 return，留下上一只精灵的文本和 UserRole 数据。现在显式清空该行所有单元格。

### P2：死代码与死字段

删除全仓库零引用的 API：`WorkbenchWindow::setSearchHandler` / `setSessionResetHandler`、`DiagnosticLogger::logPath`、`ShopExchangeCatalog::itemObject`、`ShopExchangeController::updateCatalog`、`AssetAnalysisController::recalculateOverview`、`PetRepository::preserveBackpackDetail` / `residentDetail`、`ObservationStore::checkedClock`、`ControllerCacheStorage::pendingWrite`、`CompiledShopCatalog::raceIndex`、`RoutineOverviewController::hasObservedDailyPacket`、`RecommendationModel::recommendationAt`。

删除从未被读取的字段：`DetailVersion::detailVersion`、`DetailPreparationLimits::pageBytes`、`ImageServiceOptions::backoffMilliseconds`。

删除整条自动刷新支路：`automaticTimer_`、`scheduleAutomaticRefresh()` 及其 13 处调用点，以及 `WriteSender` typedef / `setWriteSender` / `writeSender_` 分支。`RefreshTimings::automaticIntervalMs` 保留（设置项兼容），但注释改为说明没有任何东西会因为时间流逝而发起查询。

CMake 里摘掉 5 个遗留详情管线源文件，对应文件已从工作树删除。

### P3：重复实现

- 两个渲染器各自内联了一份 HTML 文档骨架和 `escaped` / `number` 助手。提取到 `src/extension/html_document.h`：共用基础样式 + 各自追加差异样式，渲染结果逐字节不变。
- loader 目标里有两份 `quoteArgument`（`main.cpp` 和 `data_root_config.cpp`）。`CommandLineToArgvW` 的反斜杠转义规则写两遍正是 bug 藏身处，提取到 `src/loader/command_line.h`。

### P4：性能

- `showDetail` 在 prepared handle、图片路径、窄屏标志三者都未变化时直接返回，不再重建并重排整份详情文档——用户可见的收益是不会再被重置滚动位置。
- 分页按钮只在页数变化时重建控件；页数不变时只更新选中态。
- `pinyinInitial` 加线程局部记忆化。每次击键都要为所有可见名字求首字母，而反复出现的就那几百个字符。
- `ImageService::localPaths` 的去重从 `QStringList::contains`（O(n²)）换成并行的哈希集合，保留原有顺序语义。
- `application_runtime.cpp` 两处 `backpackBriefs() + warehouseBriefs()` 少一次整表拷贝。

### P5：可观测性与健壮性

- `parseWarehouse` 的两条拒包诊断原先只说「duplicate instance across groups」「instance conflicts with retained group」，现在带上具体实例 ID、所在分组和被保护的分组名。
- `resizeModelViewColumns` 对非 8/9 列的表格原先静默 return，留下默认列宽。改为均分兜底。

## 三、有意没做的三项

- **`/W4` 警告等级**：这是策略变更，不配一轮警告清理就只会把干净的构建日志变成噪音，收益要等警告被逐条分诊后才出现。
- **合并两份 `textBytes`**：`pet_derivation_cache.cpp` 与 `compiled_shop_catalog.cpp` 各有一份，看着重复，实际语义不同（一个空串算 0、按 `size()`；另一个恒加 64、按 `max(size, capacity)`），分别服务不同的内存预算。合并会改变记账行为。
- **内存上限调参、`recommendation_adapter` 移除、背包视图重构**：前者需要实测重新基线，中者要重写 `recommendation_smoke.cpp`，都不适合和这批一起动。

## 四、两次误判，以及它们说明了什么

### 误判一：把在用的代码判成死代码

`TargetCompatibilityGuard::resolveEndpoint`、`InboundQueueState::retainedPackets`、`SnapshotCacheStats::fullSnapshots` / `fullBytes` / `instanceEntries`、`PacketContract::responseShape` 被删掉，编译立刻失败。原因是搜索时的源码树不完整，漏掉了 34 个测试文件。

值得记下的是**为什么这次是幸运的**：`InboundQueueState` 和 `SnapshotCacheStats` 都由 `return {a, b, c, d};` 这样的**位置聚合初始化**构造。删掉中间一个字段不会产生编译错误，只会让后面每个值静默错位赋值——只因为字段数正好对不上才被编译器拦下。

> 在这个仓库里删 struct 字段前，先确认它没有被位置聚合初始化填充。`grep` 字段名找不到写入点，不等于没有写入点。

`PacketContract::responseShape` 同理：它从不被运行时读取，但 `PacketContracts::all()` 里每一行都按位置填了它，是随契约表一起携带的文档数据。

### 误判二：把「重复」当成可以合并

把三处 `displayName` 统一成共享的 `petDisplayName` 时改变了语义。表格列和精灵窗口一直**故意**显示服务器名 `n` 而不是玩家昵称，仓库列也只用「精英 / 普通」短标签而非完整仓库名。`pet_table_model_smoke` 里专门有一条断言在守这个不变量（"nickname replaced the server name or stopped matching search"），测试当场失败。

三处实现长得一样不代表它们该是同一个函数。现在这几处都留了注释，写明「这里故意不用共享版本」及其原因。

### 误判三：`src/extension` 与 `src/domain` 的同名头文件

`src/extension/` 下有十个只有两行的转发头。其中七个转发到 `src/domain/` 里的**同名**头文件，删掉后 `#include "x.h"` 会自然落到 domain 版本上，行为不变。另外三个不是：

| 头文件 | 实际转发到 |
| --- | --- |
| `src/extension/recommendation_engine.h` | `src/application/recommendation_adapter.h` |
| `src/extension/shop_actionability.h` | `src/application/shop_legacy_adapters.h` |
| `src/extension/shop_pet_eligibility.h` | `src/application/shop_legacy_adapters.h` |

这三个的文件名和 `src/domain/` 里的头文件**撞名但内容不同**。删掉它们，`#include "recommendation_engine.h"` 不会报「找不到头文件」——它会静默改为命中 domain 版本，然后在几十行之后炸成一串「`RecommendationEngine` 不是类或命名空间名称」。`tests/algorithm_pipeline_smoke.cpp`、`tests/recommendation_smoke.cpp`、`tests/shop_exchange_smoke.cpp` 三个目标因此编译失败。

> 在这个仓库里，`src/extension` 和 `src/domain` 有十处同名头文件，而多数目标两个目录都在 include 路径上。裸名 `#include` 命中哪一个取决于目录顺序，删掉一个「看起来多余」的转发头不会产生缺失头文件的错误，只会换一个目标悄悄命中。

这三个头文件已恢复，并在文件里写明了它们不是冗余转发。真正的清理是消除文件名冲突（重命名 domain 侧，或把裸名引用改成显式相对路径），那需要单独一轮改动和回归。

## 五、验证

- 完整 Release 回归 72/72。
- `tools/check_ui_boundary.py`、`tools/check_domain_boundary.py` 均通过。
- 已部署至 `KQPetRuntime` 并在真实客户端确认界面行为。
