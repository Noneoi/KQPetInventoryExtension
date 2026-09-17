# 修复与性能优化进度（任务书 T0–T10）

本文件按任务书维护。状态只用：`Pending / Investigating / Reproduced / FixedAndTargetedTested / NotReproduced / Blocked / Verified`。

执行环境（2026-09-16）：

| 项 | 值 |
| --- | --- |
| 仓库 | `C:\Users\25982\Desktop\11\KQPetInventoryExtension` |
| 基线 commit | `4d29a6e409a619ae38b39f524aa78ff87aa50e35`（main，工作区干净） |
| 任务分支 | `fix/kq-reliability-perf`（从上述 commit 建立） |
| Qt | `D:\Qt\6.6.3\msvc2019_64` |
| MSVC | 19.51.36231（`D:\Visual Studio\VS`，VsDevCmd x64） |
| CMake / 生成器 | CMake 4.4.2 + Ninja |
| Python | 3.14.7（`C:\Users\25982\AppData\Local\Programs\Python\Python314\python.exe`） |
| 构建目录 | `build-agent-fix`（Release，453 个目标，退出码 0） |
| 证据目录 | `validation-logs\`、`C:\Users\25982\Desktop\11\build-performance-results\t0-baseline\` |

执行期环境说明：本次会话早期文件策略为受限模式时，CMake 的 Ninja 生成器在
`try_compile`/链接阶段会挂起（已在仓库外最小工程复现：`.obj` 生成后不产出 `.exe`，
ninja 线程停在 `Wait EventPairLow`）。放宽文件策略后同一命令 0.7 s 完成配置、构建退出码 0。
因此本次所有构建与测试命令都在可完整执行的权限下运行；这不改变仓库的构建脚本。

---

## T0 建立真实基线，核对官方资源证据

状态：`Verified`

实际基线：commit `4d29a6e`，`git status --short` 无输出；`git branch -a` 只有 `main` 与 `origin/main`。

已执行命令与日志：

| 命令 | 退出码 | 日志 |
| --- | --- | --- |
| `git rev-parse HEAD` / `git status --short` / `git log -1 --format=fuller` | 0 | 本文件 |
| `.\scripts\build.ps1 -QtRoot 'D:\Qt\6.6.3\msvc2019_64' -BuildDirectory .\build-agent-fix -Configuration Release` | 0 | `validation-logs\t0-build-baseline.log` |
| `ctest --test-dir .\build-agent-fix -C Release --show-only=json-v1` | 0 | `validation-logs\t0-test-list.json`（注册 71 项） |
| `ctest --test-dir .\build-agent-fix -C Release --output-on-failure` | 8 | `validation-logs\t0-ctest-baseline.log`（66 通过 / 5 失败） |
| `.\scripts\run-performance.ps1 -Executable .\build-agent-fix\bin\Release\KQAssetAnalysisPerformance.exe -Matrix Priority -Warmup 1 -Samples 3` | 2 | `validation-logs\t0-performance-baseline.log`、`C:\Users\25982\Desktop\11\build-performance-results\t0-baseline\`（脚本用 `[IO.Path]::GetFullPath` 解析相对输出目录，实际落在工作区根而非仓库内） |

基线失败分类（5 项，均为本次修改前状态）：

| 测试 | 分类 | 首个失败点 | 归属 |
| --- | --- | --- | --- |
| `ui_boundary` | 测试/审查清单过时 | 3 处 `contracts/*.h` 值类型被列为未审查包含 | T2 |
| `protocol_fixture_smoke` | 测试/夹具语义过时（合成用例按旧分项口径构造） | `detail analyzer did not produce the expected stable view model` | T2 |
| `recommendation_smoke` | 待分类（见 T2/T6） | `unknown completion generated a near-full claim` 等 3 行 | T2 |
| `analysis_cache_integration_smoke` | 待分类（T5 范围） | `one thousand updates supersede old computation ...` | T5 |
| `pet_move_controller_smoke` | 待分类（T5 范围） | 13 行移动写入/隔离断言 | T5 |

基线性能抽测：`p2000-g200-normal-d100-k8-c3-m3` 热/冷均无失败项；`p10000-g1000-sparse-d100-k8-c3-m3`
与 `p10000-g1000-full-d100-k8-c3-m3`（取消组）各项重测 `correct=false`、退出码 2。首个原因（原始样本）：

```text
outcome=InputRejected finishStage=preparation
error=实例 10004550 派生未完成：derived facts retained by consumers exhausted the budget；已保留上次分析
productionCaches.factsPeakBytesSampled=181189866 > productionLimits.factsRetainedBytes=134217728
rawLoadErrorCounts={"详情读取队列拒绝任务":123} preparationReadAdmissionRetries=246
```

这是修改前就存在的基线现象（10,000 只规模下派生 facts 账本超预算导致准备阶段被拒绝），
不是本次改动引入。未调整任何预算或数据规模来让它变绿；留待批次三（T10）与最终验收处理。

官方资源证据：本次未访问 `https://aoqi.100bt.com/play/start.xml`（未取得网络抓取授权与产物路径）。
已有冻结快照 `docs/official-pet-metadata-20260913.md`、`docs/shop-catalog-source-20260913.md`
沿用；最新线上协议未复核，T0 的“最新游戏文件辅助验证”标记为未执行。

剩余风险：真实客户端未部署（无客户端访问与操作授权），批次一的客户端验收项全部记为 NotRun。

---

## T1 缓存目录身份比较与迁移错误回退

状态：`FixedAndTargetedTested`

相关函数/调用链：

- `src/loader/data_root_config.cpp`：`validRoot()`、`resolveDataRoot()`、`prepareDataRootMigration()`、
  `clearPendingFailure()`。
- `tools/cache-manager.ps1`：`Full-Directory()`（用 `[IO.Path]::GetFullPath`）、`migrate`、`schedule-root`。
- `src/runtime/release_manifest.cpp`：`equalPath()`、`directoryTree()`、`openPinned()`（同源线索，见下）。

已测得的机制证据（本次实测）：

```text
GetShortPathNameW(C:\...\kq83b-e0eed567\LongDirectoryNameFor83Test)
  → C:\...\DSH-GD~1\KQ83B-~1\LONGDI~1
[IO.Path]::GetFullPath(短名)  → 长名（PowerShell 5.1 与 7.6 行为一致）
std::filesystem::lexically_normal() 不做文件系统解析（启动器侧 validRoot 只做词法规范化）
```

即：迁移脚本侧会把 8.3 短名按文件系统展开成长名写回配置，而启动器侧只做词法规范化，
两侧对“同一目录”可能给出不同拼写；基线用 `_wcsicmp(committed.dataRoot, pendingRoot)`
判定“配置尚未提交”，命中该差异时会走 `clearPendingFailure()`，把 `dataRoot` 写回旧目录
（数据已复制但切换被静默回退），并清掉待迁移请求。

复现（修复前，生产代码保持基线，只加测试）：
`validation-logs\t1-prefix-repro.log`

```text
Alias migration diagnostic: 缓存迁移的目录配置尚未提交。继续使用原缓存目录。本次迁移请求已取消，可在设置中重新安排。
FAIL: a long/short alias pair for one directory was rejected as an uncommitted migration
FAIL: the alias-spelled committed migration was not stable after a restart
```

修复：新增 `compareDirectoryIdentity()`（`GetFileInformationByHandleEx(FileIdInfo)`，旧式
`dwVolumeSerialNumber + nFileIndex` 仅作同方法回退且要求非零）与 `plainDirectoryPath()`
（逐级要求已存在、普通目录、非重解析点）；`prepareDataRootMigration()` 的迁移后验收改为
“配置已提交 + 目录链合规 + 两侧目录身份相同”，身份无法确认时明确报错并保留旧目录，
不再回退成字符串放行。`validRoot()`、配置文件非重解析点检查、`clearPendingFailure()` 语义保留。

改动文件：`src/loader/data_root_config.h`、`src/loader/data_root_config.cpp`、
`tests/data_root_config_smoke.cpp`、`tests/test_cache_manager.py`。

定向验证：`validation-logs\t1-targeted-ctest.log`（`data_root_config_smoke`、
`cache_manager_smoke`、`release_core_smoke`、`release_tools_smoke` 全部通过）；
`validation-logs\t1-postfix-targeted.log`。

覆盖情况：普通路径、中文与空格、有效长短别名（本机 8.3 可用，用`GetShortPathNameW`取得真实别名）、
不同目录、损坏配置、迁移失败保旧、目录越界（含互相包含）、既有重解析点拒绝（Python 侧 junction）、
成功后再次启动；身份无法确认（缺目录/普通文件/相对路径）按显式错误处理。
未覆盖：ACL 权限拒绝（构造需要修改真实目录 ACL，未做，避免遗留权限残留）；
`plainDirectoryPath()` 的重解析点分支由端点检查与 Python junction 用例覆盖，未单独造 junction 断言。

`release_manifest.cpp` 的同源线索（本次仅核查，未改动）：`directoryTree()` 要求
`GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)` 的长度与请求拼写完全一致，因此同样拒绝
8.3 别名。该检查用于发行目录的“不得跟随链接/非规范拼写”安全边界，输入来自启动器自身路径，
与数据目录迁移的用途不同，按任务书“不得机械替换”未接入同一 helper。

剩余风险：真实客户端“迁移成功后再次启动仍使用新目录”的手工验收未执行（无客户端访问授权），
标记 NotRun。


---

## T2 更新器编码与过时测试断言

状态：`FixedAndTargetedTested`

### A. 更新器编码契约

改动文件：`tools/public_data_updater.py`（`emit()`）、`tools/public_activity_exchange_updater.py`
（`emit()`）、`tools/public_routine_updater.py`（`_emit()`）。三处都是局部改到 `ensure_ascii=True`，
没有引入共享模块（避免同时改 Qt 资源打包、更新器复制清单和离线包依赖）。

契约：所有**机器消费**的进度行一律为纯 ASCII JSON（非 ASCII 以 `\uXXXX` 转义），
解码后文字与原串完全相同；文件输出（`atomic_json`）仍为 UTF-8 原样，不受影响。
理由：应用内调用链（`src/application/data_update_service.cpp` 设置 `PYTHONUTF8/PYTHONIOENCODING`，
`tools/bootstrap-public-data.ps1` 第 9–10 行也设置）当前可用，但独立运行脚本会继承控制台代码页，
cp936 下写出 GBK 字节、cp1252/ascii 下直接抛 `UnicodeEncodeError` 并丢掉 `finished` 事件。

复现（修复前，原始日志 `validation-logs\t2-encoding-prefix-repro.log`，643 行，`FAILED (failures=42, errors=3)`）：

```text
UnicodeEncodeError: 'ascii' codec can't encode characters in position 34-56: ordinal not in range(128)
UnicodeEncodeError: 'charmap' codec can't encode characters in position 34-56: character maps to <undefined>
UnicodeEncodeError: 'gbk' codec can't encode character '\U0001f680' in position 39: illegal multibyte sequence
```

新增回归：`tests/public_updater_output_encoding_test.py`（已注册 `public_updater_output_encoding_smoke`，
CMakeLists 第 226 行），覆盖 3 个发射器 × 4 种文本（中文 / emoji / 混合 / ASCII）× 5 种流编码
（ascii / cp1252 / cp936 / utf-8 / 不设置）× 管道与文件重定向；断言退出码为 0、无
`UnicodeEncodeError`、整行为 ASCII、每行可解析为 JSON、字段完整、解析后文本与原串逐一相同。
修复后该测试 3.5 s 通过。

### B. 过时测试断言（按实际失败逐项核对）

`ui_boundary`（`validation-logs` 基线日志）：失败原因是被 UI 包含的
`src/contracts/cultivation_material_inventory.h`、`src/contracts/local_stargod_statistics.h`
未列入审查清单。核对两者确为无实现的只读值类型（一个快照结构 + 一个 inline key helper /
一个统计结果结构 + `Q_DECLARE_METATYPE`），包含项仅 Qt 头文件 → 最小更新 `PORTS` 清单，
未放开 implementation 目录。`check_ui_boundary.py --self-test` 通过。

`recommendation_smoke` 三行失败，首个根因合并为一类：**合成用例按旧的星神口径构造**。

1. `unknown completion generated a near-full claim`：生产逻辑 `finishPet()` 只在
   `pet.completionKnown && 90 <= completionPercent <= 100` 时产生 `NearFullCultivation`；
   完成度未知时按设计保留有证据的 `LocalCultivation`。旧断言要求结果整体为空属过时。
   改为按结果类型断言：一行、类型为 `LocalCultivation`、`!completionKnown && !powerGapKnown`。
2. `proven one-unit exchange quantity ...` / `proven gap closure did not rank ...`：
   诊断显示候选类型是 `ConditionUnknown`、`eligibility` 未知，原因是测试宠物缺少本宠星神背包
   （`sgsp`）与完整分项，`stargodAcquisitionKnown` 无法成立，`oneRedStargod()` 只好返回 Unknown，
   于是 `provenGap` 分支（要求 ReadyNow）永不进入。按任务书“依据明确业务语义更新夹具”补全
   raw 回复（`sgs = "0:8#0:8"` 两个空普通槽 + `sgsp = []`），记录镜像（`missingRedStars`、
   `stargodSlotsKnown`）保持不变；断言数值仍未改动（1 / 1，closesKnownGap 且 actionableCount==2）。
   该用例现在通过。

`protocol_fixture_smoke` 七行失败，首个根因同样是一类：**合成/冻结夹具与 v2 已确认协议口径不一致**。

- 被包含的 `real_relationship_v1_sanitized.json` 只有 6 个战力分项、没有 `sgsp`。
  按 `docs/精灵字段语义映射.md`（“完整分项有以上 11 个键，不适用项返回 0；缺失项不能等同于
  不适用或满培养”）与 `docs/pet-power-composition.md`（“当前与极限必须都完整、键一致、
  数值有效”），该冻结样本的本地总数必须保持未知。改为：对原样本断言“未知不被算成合计”
  （`!hasCurrent && current==0 && !hasHighest && highest==0`），另按同一观测补全 5 个不适用分项
  为显式 0 并补 `sgsp`，再断言原有数值 14547 / 29400 / 30750 —— 补全后**完整复现**旧数值，
  证明差异只来自完整性门槛，而不是算式改变。
- `backpackStargodPet` / `sixSlotPet` / `threeSlotPet`：旧期望按“按星神定义去重、按满级计算”给出
  （availableStars=7、missingStars=1、bestOrdinary=3690、changeable=500）。按已确认规则
  （同类 `type` 不能重复占普通槽、槽等级取实际等级、万变星神不占普通槽类型配额）用冻结目录手工核算：
  本宠可用的普通类型为 6（18 与 70 同类）、7 个槽还缺 1、槽位等级为 1（红色 140/等级、蓝色 40/等级），
  故正确值为 availableStars=6、missingStars=1、非满、bestOrdinary=740、changeable=140（背包 80 为红星）、
  currentStargod=880、highest=30750。六槽组 missingStars=3，三槽组 availableStars=1/missingStars=2。
- `locallyCalculatedHighest` / `backpackRedReplacement`：补全分项后仍缺“星轮适用性”，
  原因是合成宠物没有 `astrolabe` 序列（`hasHighest` 必须等 `astrolabeApplicabilityKnown`）。
  旧夹具的 `asv=150` 与任何真实节点组合都不自洽（冻结目录节点战力只有 190/210/400/700）。
  改为使用冻结目录中 3 个 190 战力外圈节点（`350:1:1#351:1:1#352:1:1`）并令
  `czdlv.asv=720`（570 节点 + 150 灵初突破加成）、`mzdlv.asv=570`（官方极限不含突破加成），
  于是当前/至高均为 25400+720+5200=31320，且服务器 `zdl=99999`、`mzdlv.sgv=9999` 被忽略，
  该用例的原本意图（至高不依赖服务器值）反而更直接地被验证。
- 差距文案断言：旧期望的聚合措辞（“等级与基础成长差 100 战力”“天迹星轮还有 1 个未点亮”等）
  在当前渲染器里已不存在；渲染器改用逐分项（“当前 … / 官方极限分项 … / 至高分项 … / 尚缺 …”）
  与逐节点呈现，且读取的是 `battlePower.components`（旧用例只填了 `componentGaps`）。
  按当前已确认呈现更新断言，保留全部语义点（分项缺口、专属元魂未觉醒、神源兽星/阶缺口、
  未点亮与需材料、专属节点材料仅在未点亮时显示、突破状态），并保持负向断言不变。
  记录：`activatedCount/selectedCount` 当前不在界面上作为文案输出，旧聚合句已移除（属呈现变化，
  未新增功能）。

改动文件：`CMakeLists.txt`、`tools/check_ui_boundary.py`、`tools/public_*.py`、
`tests/public_updater_output_encoding_test.py`（新增）、`tests/recommendation_smoke.cpp`、
`tests/fixture_smoke.cpp`。三个失败用例的 `FAIL` 行改为“同样信息量更精确”的按类型/按字段断言，
并新增失败时的现场诊断输出（`reportRows` / `reportPower` / `reportBattlePower`），不删除任何约束。

定向与全量验证：`validation-logs\t2-recommendation-after.log`、`validation-logs\t2-fixture-final.log`、
`validation-logs\t2-full-ctest.log`（72 项注册测试，70 通过；剩余 2 项为 T5 范围的基线失败，
无新增失败、无删除测试、无 `continue-on-error`）。

剩余风险：`analysis_cache_integration_smoke` 与 `pet_move_controller_smoke` 仍失败，属批次二 T5；
编码契约没有覆盖不经过这三个 `emit()` 的人类可读输出（如 `generate_shop_exchange_data.py` 的
`wrote ...`），它们不是机器消费行，未改动。

---

## T3 日常/每周玩法汇总完整性

状态：`FixedAndTargetedTested`

相关函数/调用链：`src/extension/asset_analyzer.cpp:analyzeRoutine()`、
`src/domain/asset_analysis_types.h:AccountAssetOverview`、
`src/extension/routine_overview_controller.h`（分组状态）、
`src/extension/asset_analysis_window.cpp:rebuildOverview()`。

问题（已复现）：基线把“至少一个来源有效”当成总量已知。`add()` 一次有效就把
`todayOpportunityKnown` 置真，其余来源缺失/无效/周期未核验只是被跳过，界面因此显示
`玩法剩余 N`，把部分合计当成整期总量。复现（修复前）：`validation-logs\t3-prefix-repro.log`
——5 个只读来源只返回 1 个且其周期已核验时，`todayOpportunityKnown` 仍为真。

修复：

- 新增 `RoutineCompleteness{Unknown,Partial,Complete,Overflow}` 与
  `RoutineOpportunitySummary`（已确认小计 `total`、`confirmedSources`、`expectedSources`、
  `pendingSources`（含原因）、`observedAt`）；`AccountAssetOverview` 增加
  `todayOpportunities`/`weekOpportunities`。`todayOpportunityKnown`/`weekOpportunityKnown`
  的含义明确为“整期总量已完整确认”。
- `analyzeRoutine()` 改为逐个独立贡献项求值：每个来源分别检查
  `fieldState(group)`（Value/Empty/Invalid/Missing）与 `group:activity` 周期有效性，
  再解析自身数值。`Empty` 记为“已确认的 0”，缺失与真实 0 继续分开。
- 应有来源 = 本次分析覆盖的分组（`RoutineOverviewController::hasRecordedState()`，
  新访问器）。未被任何查询覆盖的分组不参与期望，因此未开启/不适用的玩法不会造成永久
  Partial；被动观察的竞技场 `16_24_A:zao1|zao2` 只有游戏自身返回时才计入期望。
- 溢出（合计超出 int 可表示范围）单独成为 `Overflow`，此时 `total` 不给出“可靠下限”。
- 发现并一并修正：原先竞技场使用 `16_24_A:activity` 作为周期键，而该键从不产生
  （分组按 `zao1`/`zao2` 登记），导致竞技场次数实际永不计入；现按字段键判定。
- UI：`今日/本周任务 / 玩法剩余` 行改为
  `玩法剩余 N 次`（完整）/`已确认 N 次，另有 M 项未更新`（部分）/
  `玩法次数未确认`（未知）/`当前无适用玩法`（零适用）/`玩法次数合计无效（超出可表示范围）`
  （溢出），未更新来源列入行提示。

改动文件：`src/domain/asset_analysis_types.h`、`src/extension/asset_analyzer.cpp`、
`src/extension/routine_overview_controller.h`、`src/extension/asset_analysis_window.cpp`、
`tests/routine_overview_smoke.cpp`、`tests/workbench_ui_preview.cpp`。

回归（`tests/routine_overview_smoke.cpp`，用真实 `RoutineOverviewController` 三个刷新周期
＋合成周期证据，不联网、不读真实账号）：部分（1 个来源无效、4 个有效 → Partial 45、
pending 1，周常仍 Complete 6）、全部有效（Complete 47/6）、竞技场单字段返回（6/7 部分）、
恢复后 Complete、溢出（Overflow 且 total=0）、全部缺失（expected=0/Unknown）、
周期未核验（Unknown）、切号清空旧覆盖。日志：`validation-logs\t3-postfix.log`（退出码 0）、
`validation-logs\t3-targeted-ctest.log`（7 项相关测试全通过）。

剩余风险：未覆盖跨日/跨周的真实周期翻转（依赖系统时钟推进，测试用固定时钟；
`observation_freshness_smoke` 已覆盖周期边界过期语义）；离线缓存路径下
`fieldStates_` 不随缓存恢复，因此重启后未刷新时汇总保持 Unknown（与“只读旧观察”一致），
未在本次改动中改变。

---

## T4 图片显示与持久化完成分离

状态：`FixedAndTargetedTested`

相关函数/调用链：`src/application/image_service.cpp` 的 `ImageIo::save()`、`completeDecode()`、
`requestBatch()`/`pumpBatch()`、`report()`、RAM 命中分支；`src/application/image_service.h`
的结果合约；`tests/image_service_smoke.cpp`。

问题（已复现）：`ImageIo::save()` 返回 void，路径/目录/锁/打开/写入/提交失败都直接 return；
`completeDecode()` 投递保存后立刻 `report(Ready)`，批量监听 `completed(Ready)` 就累加完成。
于是“能显示”被当成“已落盘”：缓存目录不可写时批量仍报 0 失败。
复现（修复前，只使用旧 API 的断言）：`validation-logs\t4-prefix-repro.log`
——`FAIL: a batch entry that could not be written was counted as a persisted image`（退出码 1）。

修复：

- 新增 `ImagePersistence{Saved,AlreadyValid,Failed,Cancelled}` 与 `ImagePersistenceResult`
  （含显式原因、`metadataSaved`、`batchCounted`）以及 `persistenceCompleted` 信号；
  `ImageMemoryUsage` 增加 `persistedImages`/`persistenceFailures`。
- `save()` 改为返回结果：路径无效、目录不可写、锁冲突、打开失败、写入不完整、提交失败、
  元数据未提交、取消（提交点前后区分）都给出明确原因，不再静默 return。
  保留 QSaveFile 原子替换、`safeExistingChain` 路径/重解析点检查与 QLockFile。
- 元数据提交失败不再宣称新版本：栅格已提交（保留文件用于诊断）但结果记为 Failed，
  并说明“下次检查会重新获取”。
- 交互预览继续“解码成功即可显示”：预览在 `report(Ready)` 后立即发布，不等磁盘。
- 批量记账改为“终态唯一且基于持久化结果”：`completed(Ready)` 不再计成功；
  持久化回执 Saved/AlreadyValid 计成功，Failed/Cancelled 计失败；
  显示层终态（Rejected/Unavailable/BudgetExceeded/Closed）直接计失败。
- RAM 命中不再等于已落盘：批次键命中 RAM 时会通过 I/O 确认磁盘文件仍存在且是可识别图片
  （存在、非空、大小受限、文件头合法），否则失败。
- 每批有 `batchGeneration`，每个作业有 `jobId`；旧批回执不决定新批
  （`batchCounted=false`），同一键只记一次终态。
- 批量不再接收图标（内置资源无账号缓存项），改为在 `requestBatch()` 明确计为失败，
  避免等待不存在的回执。
- I/O 投递被拒时立即给出可见失败（“I/O 队列拒绝…”），不做静默跳过。

故障注入回归（`tests/image_service_smoke.cpp`，注入式 I/O 闸门/序号拒绝 + 目录占位 + 锁占用）：
预览先显示后落盘、迁移保存成功且新服务可本地读取、不可写目标仍显示但报失败、锁冲突失败、
保存投递被拒失败、元数据失败（栅格保留、结果失败）、RAM 命中但文件被删（批量计失败）、
已有有效文件计 AlreadyValid、取消中的保存不报成功、旧批回执不污染新批、服务析构后无回调。
日志：`validation-logs\t4-targeted.log`、`t4-postfix.log`（退出码 0）、`t4-prefix-repro.log`。

剩余风险：`save()` 提交点之前的取消分支在本套件中不能确定性注入（需要取消恰好落在
QSaveFile 写入窗口内）；已用“取消发生在保存开始前”的用例与代码内检查覆盖，其余路径
（I/O 拒绝、目录/锁/元数据失败）均有确定性用例。真实磁盘故障与真实客户端图片流程未验证。

---

## T5 移动状态机与旧结果发布回归

状态：B 部分 `Verified`；A 部分 `Blocked`（已定位首个根因，未做未经等价性论证的修改）

### B. 大量更新只发布当前版本（`analysis_cache_integration_smoke`）

原失败行：“one thousand updates supersede old computation and publish only the newest version”。
诊断（新增现场输出）：`projectedRevision == latestRevision == 1005`（版本正确）、
`pending=0 active=0 resident=3 rejected=0`，但 `projectedPower=0`。
根因：断言用 `facts.asset.currentPower`（本地可达战力）作为新版本判据，而该夹具的
`czdlv` 只带一个分项，按已确认规则本地总数必须保持未知（与 T2/T3 同一类过时断言）。
改为按“输入版本 + 该版本的观察值”断言：`facts->key.record == latestVersion` 且
`battlePower.serverCurrent == 10000`，并保留失败现场输出与原有的计算/驻留上限断言
（`computations <= burstStart+2`、`residentEntries <= 3`、`chargedBytes <= limits`）。
日志：`validation-logs\t5-analysis-cache.log`（退出码 0）。

### A. 取消后再次移动（`pet_move_controller_smoke`）

状态：`Verified`（根因已在生产代码中最小修复，14 条失败全部消失，全套 72 项通过）

修改前基线：3 次运行均为 14 条失败且逐条一致（`t5-move-run1..3.log`），首个失败为
“full-pack replacement result is incorrect”。

根因（现场输出 `t5a-baseline-run1.log`、`t5a-instrumented*.log`）：

1. 首个失败现场（首次复现）：
   ```text
   PROMPT 2 rev=20 eligible=2
   STATUS rev=20 detailReq=1 pendingReads=0: 正在保存实例 3 的必要详情；保存成功前不会提交移动请求……
   STATUS rev=20 detailReq=1 pendingReads=1: 正在保存移动意图；确认写入磁盘后才会调用宿主……
   [REV1487] id=1 knownOrig=0 prevComplete=0 prevPersisted=0 curEmpty=0 eqRoster=1 added= changed= removed=
   STATUS rev=21 detailReq=1 pendingReads=0: 意图保存期间列表或会话已失效，未提交请求。
   ```
   即 `PetRepository::applyDetailCache()`（磁盘详情读取完成路径）对**逐字节等同**的重发
   无条件 `++inventoryRevision_`，使写意图落盘完成时的 `movePreflightStillValid()` 判定失败。
2. 用 `movePreflightStillValid()` 现场打印逐条核对剩余 4 条失败（`t5a-instrumented5..7.log`）：
   全部为 `auth/account/epoch/storage=1`、仅 `rev=captured+1`，触发点仍是该磁盘重载；差异仅
   限于被名册副本遮蔽的原始字段（`changed=_position`、`removed=inFormation`）。

修复（`src/extension/pet_repository.cpp`，`applyDetailCache()`）：发布前记录消费者可读的三项
事实——名册副本 `rosterBriefBefore`、合并视图 `mergedBefore`（`detailFor()`，即
`PetMovePolicy::restriction()`/分析种子实际读取的对象）、以及记录的 `complete`/`sourceKnown`
证据；只有在其中任一项确实变化（或该实例不在名册中）时才 `++inventoryRevision_`。原字段
级比较（`rawObjectBefore != input.object`）经实测过严：名册拥有的 `_position` 等字段差异对
`detailFor()` 不可见，故最终条件不含原对象。

回归测试（先失败后通过）：
- 新增 `tests/repository_smoke.cpp::diskDetailReloadRegression`：登录 → 首次部分观测被
  “only-if-missing”落盘 → 第 2 次相同部分观测触发写被取代（Superseded），仓库转而执行
  “Load that original now”，断言该次磁盘重载既未改变 `backpackPet()` 也未改动事实修订。
  修改前：`t5a-prefix-final.log` → `revisionAfterLists=5 revisionAfterReload=6`，
  `FAIL: identical disk detail facts unnecessarily invalidated the fact revision`。
  修改后：`t5a-fix-repository.log` → `5 → 5`，退出码 0。
- `pet_move_controller_smoke`：14 → 0 条失败（`t5a-move-after-fix-run1/2.log`），并连跑 3 次
  与仓库回归同结果（`t5a-move-final-run1..3.log`、`t5a-repository-final-run1..3.log`）。
- 全套：`t5a-full-ctest.log` → 72/72 通过（这是本工作流首份全绿记录；T0 基线为 5 项失败）。

等价性论证与残余风险：`inventoryRevision()` 的消费者是移动写前置、分析输入戳
（`asset_analysis_controller`）、发布器（`inventory_publisher`）与快照存储。前两者读取
`briefFor()`/`detailFor()` 与记录键（`PetRecordKey`/`detailMemoryRevision`）；磁盘重载对已
知原记录保留原键，其派生/重算由记录可用性驱动而非事实修订。因此“名册副本 + 合并视图 +
完整性/来源证据均不变”时不再自增，不会让可见事实或分析输入变新；`complete`/`sourceKnown`
变化仍必然自增（保持“不把未知当已知”的既有语义，`repository_smoke.cpp:591` 的
“new detail facts must invalidate”断言仍通过）。残余风险：直接读取原对象（`rawRecordHandle`
的 `object()`）且只关心被名册遮蔽字段的消费者不会收到事实修订变化，但其可见内容未变。

---

## T6 推荐数量、容量与预算一致性

状态：`Verified`（回归补齐；生产代码无需修改）

核对结论（`src/domain/recommendation_engine.cpp`）：`reserveOutput()` 按 P 预留、
`finishPet()` 在“商店行 + 本地星神行动行”并存时最多每宠 2 行，容器按需有界增长，
`refreshCharge()` 按 `result.capacity()` 记账，`step()` 每轮用
`resultChargedBytes > resultBudgetBytes` 立即取消且 `takeResults()` 在非 Complete 时返回空。
即现状已是任务书允许的“合理有界增长”，且额外行通过 capacity 计入、未按 size 少记。

新增回归（`tests/recommendation_smoke.cpp`）：4 只带“已有星神待装备”行动的精灵 + 2 个商品时，
结果为 8 行（4 商店 + 4 本地）、资产总览仍完整保留 4 只；断言
`resultRowCapacityBytes >= rows*sizeof(row)` 且
`resultChargedBytes == rowHeap + rowCapacity + overviewHeap + overviewCapacity`；
同一输入用只够 P 行的预算运行时返回 `ResultBudgetExceeded` 且结果为空（无半成品发布）。
日志：`validation-logs\t6-recommendation.log`（退出码 0）。

未改生产代码的原因：实测未发现“容量/预算不一致”或“提前超预算”现象；按任务书
“没有可测收益的复杂改动不合入”，未把预留机械改成 2P。剩余风险：`compactFacts`
（preparedFacts 复用）路径下额外行的实际数量未单独测量，但其记账同样走 capacity。

---

## T7 资产概览局部更新

状态：`FixedAndTargetedTested`

相关函数/调用链：`src/extension/asset_analysis_window.cpp` 的 `rebuildOverview()`、
`refreshInventory()`、`refreshRoutineSummary()`、`addOverviewRow()`。

基线成本（代码核对）：`rebuildOverview()` 每次都 `overviewTable_->setRowCount(0)` 重建 17 行，
并对 4 个分类过滤器各做一次全仓扫描（`countFilter()`）；日常摘要变化也走同一条整表重建路径。

修复：

- `overviewFilterCounts()`：一次遍历 `overview_.pets` 同时算出 4 个分类计数并按分析结果缓存；
  缓存只在分析结果变化处失效（`applyAnalysis()`、`refreshAccountAnalysis()`、`resetSessionContext()`）。
  列表摘要变化（`refreshInventory()`）不影响这些计数，因此不再重算。
- `addOverviewRow()`：改为按标签定位并**原地更新**单元格，不再整表重建；行点击动作、
  选择与滚动位置因此保留；行集合仍为固定的 17 行。
- 新增 `updateRoutineOverviewRows()`：`refreshRoutineSummary()` 只改写今日/本周两行的数值与提示，
  不扫描精灵、不重建表格。
- 新增只读计数 `overviewPetScanCount()` 供预览自测断言“仅日常变化时全仓扫描为 0”。

回归（`tests/asset_analysis_ui_preview.cpp` 自测）：日常摘要刷新后扫描计数不变、行数与
首行点击动作不变、计数单元格不变；分类计数与对 `controller->overview().pets` 的独立重算一致；
新分析结果到达时扫描恰好 +1、行数恢复；`resetSessionContext()` 后行被清空且日常刷新不会复活旧行，
再次分析后恢复。日志：`validation-logs\t7-ui-preview.log`、`t7-full-ctest.log`
（全量 72 项 71 通过，仅剩 T5-A）。

剩余风险：`refreshInventory()` 仍会重写全部单元格文本（未做单元格级脏检查）；
本任务只消除了重复扫描与整表重建，未改动表格其它行为。

---

## T8 库存快照发布与写时复制成本

状态：`Verified`（实测后判定无需生产改动）

先测量后改动。新增 `analysis_cache_integration_smoke` 的发布突发测量：200 次详情更新
（每次一个事件循环回合，含 1 ms 间隔，总计约 3.08 s）后读取投影收到的发布数与
当前版本。

实测结果（`validation-logs\t8-final-run1..3.log`，三次一致）：

```text
publisher burst: updates=200 publications=1 elapsedMs=3080 changedIds=0 full=1
```

即现有零延时合并（`scheduled_` + `QTimer::singleShot(0)`）已经把整段突发合并为
**1 次发布**，且该发布是成员全量发布（包含全部记录版本），最终版本与仓库最新版本一致。

候选项与判定：

1. “有上限的发布合并窗口（16 ms）+ 选中详情优先”（已实现并实测）：同条件下同样是
   1 次发布，没有可重复改善，而且会额外增加最多 16 ms 的详情可见延迟；
   按任务书“没有可测收益的复杂改动不合入”**已回退**。
2. “仅在内容变化时写入共享容器”（跳过无变化写入）：语义上是严格 no-op 规避，
   但实测同样没有可观测收益，并且会让本测试中一段依赖时序的既有场景
   （250 ms IO 阻塞下的读取准入重试）出现不稳定（3 次中 2 次失败，而基线 3/3 稳定）；
   已一并回退，保持基线行为。

验收对照：最终值正确（发布快照的记录版本 = 仓库最新版本）、发布数有界（200 次更新 → 1 次发布）、
账号切换立即失效（同一测试内既有的 “account transition … cannot publish old-account facts” 断言）、
滚动/选择不重置（未改动模型与视图绑定）。发布成本的可重复改善未能测得，
按任务书不引入无收益的复杂改动；`chunked snapshot` 后续设计也未实施。

剩余风险：本测量在离线夹具（1 个实例 200 次更新）上完成，未覆盖多实例、真实网络节奏与
GUI 绘制并发的组合；未测量进程级分配量（只有发布计数与耗时）。

---

## T9 同源图片任务复用

状态：`FixedAndTargetedTested`（重复下载已消除；在途共享源任务表已实施并通过定向回归）

复现（修改前，`validation-logs\t9-duplicate.log`、`t9-prefix-repro.log`）：写入一张新索引指向未访问过的
URL，对同一 visualKey 请求 64×64 与 300×300：

```text
same-source downloads for two render sizes: 2 (receipts 1/1)
in-flight join downloads: 2
```

实施（`src/application/image_service.cpp`）：

- 新增 `SourceTask`（在途源任务表），键为 `imageSourceIdentity(request)|resourceVersion`：属性图标/星神
  图标用数字身份，其余用 visualKey；**输出尺寸与 devicePixelRatio 不参与**。任务持有请求身份、候选名与
  刷新标志、解析出的 URL/`sourceRevision`、本地候选游标、已获取的编码字节与编码预算租约、消费者集合。
- `Impl::sources` + `pendingSource`：`request()` 计算 `sourceKey`；`pump()` 先把等待作业挂到已有源任务
  （已就绪的直接复用字节，获取中的追加为消费者），再在上限内为每个源启动一次 `ImageIo::fetch`。
- 取消按消费者引用计数：`releaseSource()` 在作业终态移除其 jobId，集合为空才取消并中止在途读取/下载/
  提取；`cancelBatch()` 只让批次条目退出共享任务，不中止预览仍在等待的获取。
- 版本隔离：`refreshChangedSource` 用带 `|refresh` 后缀的源键（不复用更新前的字节）；`reloadImageIndex()`
  清空源表并标记取消，迟到完成按指针身份判定丢弃；`shutdown()` 同样取消清空。
- 解码失败回退：字节不可用时按“本地候选游标 + 强制联网”重建源任务（游标推进），并把该源的其它消费者
  一并退回等待，既不重复读同一坏候选也不死循环。
- 落盘：每个源只有第一个到达落盘的消费者执行写入，其余消费者在其回执后执行一次 `verify()`，因此每个
  渲染尺寸仍各有一份明确的 `persistenceCompleted`，两个尺寸不会争抢同一缓存文件与锁。
- 失败记录：解码判定不可用时的失败回退按“消费者侧已释放”强制写盘，否则共享源释放后 `.failure.json`
  丢失，重启会重复下载（实测到的失败原因）。

回归（先失败后通过，`tests/image_service_smoke.cpp`）：同源两尺寸断言由固定 2 收紧为 `downloads == 1`
并断言两个尺寸各自收到 `Saved/AlreadyValid` 回执；新增“在途加入 + 批次取消”夹具（`/hold-join` 150ms），
断言预览与后加入尺寸都 Ready 且服务器只收到 1 次请求。修改前 `t9-prefix-repro.log` 两条断言失败，修改后
`t9-final-run1/2.log` → `1 (receipts 1/1)`、`in-flight join downloads: 1`，退出码 0；全套
`t9-full-ctest-2.log` 为 72/72。

残余风险：多消费者并发峰值（多个尺寸同时抢同一源）与 swf 提取子进程启动次数的真实分布未单独测量；
共享字节仍受单份 `encodedImageBytes` 预算约束（原实现按消费者分别计费）。

### 已知不稳定用例（与本批改动无关，已核实）

`analysis_cache_integration_smoke` 的读压力场景（515 只精灵、`PetRecordCacheLimits::maximumRecords = 1`、
256 深读队列、20s 预算）在当前机器上时而无法在预算内完成：`t9-analysis-probe-instrumented.log` 显示
`running=1 ... protected=1 resident=1 evicted=286 refused=2150 retries=2543`，即每次发布都因单记录缓存
被占用而被拒，重试退避把读队列顶满；同一二进制另一次运行在 ~21s 内通过。
已核实与本工作无关：把 `pet_repository.cpp` 回退到 `e2e3281`（无 T5-A 修改）后同样 3/3 失败
（`t9-prefixT5a-analysis1..3.log`），T9 改动只涉及图片服务，该用例不使用 ImageService。
结论文本：**预存在的时序敏感用例，需单独定位（不在 T9/T5-A 范围）**。

---

## T10 冷分析专项

状态：`Verified`（两组规模的阶段指标已读取并定位；未发现重复解析或失效任务，故不做生产改动）

已记录的剖析入口：`scripts/run-performance.ps1 -Matrix Priority -Warmup 1 -Samples 3`
（阶段指标见 `docs/performance-benchmark-v2.md`：`preparationToFrozenInputNs`、
`coreRawJsonDecodeNs`、`cacheRawDeriveActiveWallNs`、`catalogCompileNs`、
`candidateComputeNs`、`sortNs`、`guiModelCommitNs`、`snapshotSaveNs` 等）。

批次三样本：`validation-logs\t10-performance.log`、`C:\Users\25982\Desktop\11\build-performance-results\batch3-t10\`
（逐样本 jsonl + `summary.json`）。按任务书要求逐项读取两组规模的阶段指标：

| 指标（p50，1 样本用单值） | p2000-g200 warm | p2000-g200 cold | p10000-g1000 warm | p10000-g1000 cold |
| --- | --- | --- | --- | --- |
| outcome | Published(3) | Published | InputRejected(3) | InputRejected |
| preparationToFrozenInputNs | 31.8 ms | 5.80 s | 21.5 s | 24.1 s |
| coreRawJsonDecodeNs | 0 | 157 ms | 583 ms | 632 ms |
| coreRawApplyNs | 0 | 243 ms | 966 ms | 1.07 s |
| cacheRawDeriveActiveWallNs | 0 | 1.16 s | 4.24 s | 4.64 s |
| cacheComputeActiveWallNs | 0 | 2.02 s | 7.35 s | 8.04 s |
| rawDiskReadActiveWallNs | 0 | 3.61 s | 13.9 s | 15.7 s |
| rawDiskReadQueueWaitNs（累计） | 0 | 22.0 s | 84.3 s | 95.1 s |
| readFiles / readBytes | 0 | 1988 / 16.5 MB | 7173 / 59.7 MB | 7726 / 64.3 MB |
| cacheComputations / cacheHits | 0 / 2000 | 2000 / 0 | 7211 / 375 | 7586 / 0 |
| candidateComputeNs | 53.0 ms | 61.4 ms | — | — |
| snapshotSaveNs | 28.0 ms | 29.2 ms | — | — |
| sortNs | 2.18 ms | 2.69 ms | 0 | 0 |

结论（是否可去重）：

1. **没有重复解析**：`readFiles` 与精灵数同阶（1988/2000、7173/7586），`cacheComputations`
   同样与精灵数 1:1（2000/2000、7586/7586），warm 组 `cacheHits == 2000` 表示全部命中缓存、
   未重算。每宠只有一次原文读取、一次 JSON 解析、一次派生，没有可去重的重复工作。
2. **没有失效任务残留**：p10000 的 `InputRejected` 出现在 `finishStage=preparation`，原因是
   派生 facts 账本超预算（`factsPeakBytesSampled` > `factsRetainedBytes = 134217728`），
   与 T0 基线一致，属**预算/工作集**判定而非耗时热点；`sortNs/snapshotSaveNs/candidateComputeNs`
   在该组为空（未进入候选阶段），也说明没有“先算再丢”的浪费。
3. **耗时结构**：单次详情读取的活动 I/O 约 1.8–1.9 ms，而在队列中等待约 11 ms/次
   （p2000 cold 22.0 s/1988、p10000 84.3 s/7173）；派生约 0.58 ms/宠（1.16 s/2000、4.24 s/7211），
   应用与解析分别约 122 µs、79 µs/文件。即冷分析时间由**读队列等待**与**派生**两项构成，
   两者都随规模线性，没有超线性放大点。

不改动的理由：唯一“缩短墙钟”的手段是提高读取并发/预算或替换战力匹配算法；前者属任务书禁止的
“为达标扩大预算或并发上限”，后者需要先做等价性证明，而当前指标并未显示算法热点
（派生单宠成本在两组规模下一致：578 µs vs 591 µs）。因此按“无可测收益的复杂改动不合入”保持现状。

批次四同条件复跑见下文“批次四验收”。

---

## T3–T10

状态：T3/T4/T6 `Verified`、T5-A/T5-B `Verified`、T7/T9 `FixedAndTargetedTested`、T8 `Verified`
（实测无收益、不改生产代码）、T10 `Verified`（指标已读、无可去重项）

---

## 批次四：全量重建、全量回归、性能对照、客户端验收

状态：构建/回归/性能对照**已完成**；真实客户端验收**未完成**（10 项全部 `NotRun`）

### 1. 全量重建

`cmake --build build-agent-fix`（Release，MSVC 19.51 + Ninja）从 `070e2de` 的完整树重建，退出码 0。
最终二进制：

- `build-agent-fix/bin/Release/KQPetInventory.dll`（5396992 字节）
  sha256 `44CB7669F36E44A76E3D5BF899ED72A2577309F733EBE740870EA2BD02804425`
- `build-agent-fix/bin/Release/KQPetLauncher.exe`（309760 字节）
  sha256 `A5C12F13CD0411E304A12060622BCF179D36B643F291D7724A02573EF8430541`

### 2. 全量回归

- `validation-logs\t9-full-ctest-2.log`（`ctest -j 2`，同一份二进制）：**72/72 通过**。
- `validation-logs\batch4-final-ctest.log`（`ctest -j 2`，最终提交，仅文档提交使二进制不变）：70/72，
  两个失败均为**本机时序/负载相关**的旧用例（见下）。
- 定向复跑：`t5a-move-final-run1..3.log`（移动 14→0 条失败，3/3 一致）、
  `t5a-repository-final-run1..3.log`（磁盘重载不再自增）、`t9-final-run1/2.log`
  （同源 1 次下载、在途加入 1 次下载）、`t7-ui-preview.log`、`t6-recommendation.log`、
  `batch2-targeted-ctest.log`。

#### 两个时序相关旧用例（已核实与本次改动无关）

| 用例 | 现象 | 证据与判定 |
| --- | --- | --- |
| `analysis_cache_integration_smoke` | 读压力场景 20s 内未完成（`running=1 protected=1 resident=1 evicted=286 refused=2150 retries=2543`） | 回退 `pet_repository.cpp` 到 `e2e3281` 后同样 3/3 失败（`t9-prefixT5a-analysis1..3.log`）；该用例不使用 ImageService；同一二进制早前通过（13–21s）。详见 T9 节 |
| `shop_ui_preview_smoke` | ctest `TIMEOUT 15` 超时，且**stdout 无任何输出**即被挂起 | 直接运行 8.3s、退出码 0 且自检输出 `SWITCH: first_call_ms=0 heartbeat_pulses=2982 membership_cache_hits=6 requests=0`；把 stdout 接管道后同一二进制变成 **17.8s**（超过 15s）。即用例耗时对“stdout 是否被管道消费”与本机速度敏感，ctest 恒为管道 |

结论：两例都是预存在的时间预算边界用例，本机当前速度比早前全量运行慢约 1.4–1.7×（`shop_ui_preview`
由 8.3s 升到 17.8s、`analysis_cache_integration` 由 13s 升到 24s），因此落在线界之外。不计入本次
改动的回归结论，但**不隐藏**：它们就是当前仍失败的旧用例。

### 3. 性能对照（同参数：`-Matrix Priority -Warmup 1 -Samples 3`）

原始样本：`build-performance-results\t0-baseline`、`batch3-t10`、`batch4`
（`validation-logs\batch4-performance.log` 为本次执行的逐用例摘要）。p50 对照：

| 用例 | 指标 | T0 基线 | 批次三 | 批次四 |
| --- | --- | --- | --- | --- |
| p2000-g200 warm | preparationToFrozenInputNs | 43.4 ms | 31.8 ms | 23.6 ms |
| p2000-g200 warm | candidateComputeNs | 69.1 ms | 53.0 ms | 38.0 ms |
| p2000-g200 warm | snapshotSaveNs | 39.7 ms | 28.0 ms | 23.1 ms |
| p2000-g200 warm | sampleThroughPersistenceNs | 209.1 ms | 144.8 ms | 114.0 ms |
| p2000-g200 cold | preparationToFrozenInputNs | 7.07 s | 5.80 s | 5.16 s |
| p10000-g1000 warm | preparationToFrozenInputNs | 22.9 s | 21.5 s | 18.1 s |
| p10000-g1000 warm | cacheRawDeriveActiveWallNs | 4.57 s | 4.24 s | 3.64 s |
| p10000-g1000 cold | preparationToFrozenInputNs | 23.3 s | 24.1 s | 19.2 s |

输出正确性：p2000 两组 `Published`；p10000 两组仍为 `InputRejected`（facts 账本超预算，与基线
一致），未出现新的失败类别。**归因说明**：阶段指标是墙钟量，批次三与批次四的差异不能归因于本批
改动（改动目标是 UI 局部更新、图片复用与移动前置修复），此处只声明“同条件复跑无退化”。

### 4. 真实客户端验收（未完成：无客户端访问与操作授权）

未获得客户端访问/操作授权，按任务书不自行发送任何游戏写请求，以下 10 项全部 `NotRun`：

| # | 项目 | 状态 |
| --- | --- | --- |
| 1 | 启动与各页面打开、离线已有内容读取 | NotRun |
| 2 | 刷新背包/仓库与选中详情、重启后详情缓存恢复 | NotRun |
| 3 | 手动刷新材料（普通/空闲源兽、部分失败保旧、已装备不算空闲） | NotRun |
| 4 | 本地红星统计范围与未完整缓存标注 | NotRun |
| 5 | 切号/同账号重登的旧数据与迟到回调隔离 | NotRun |
| 6 | 商店预览初始化、切换、排序筛选不触发额外全仓重算 | NotRun |
| 7 | 人工缓存队列拥塞复验（排队失败可恢复，不永久降级为缺失） | NotRun |
| 8 | 图片批量下载与预览并发、取消/暂停、保存失败可见、重启可读 | NotRun |
| 9 | 缓存迁移成功后再次启动仍用新目录且原目录保留 | NotRun |
| 10 | 移动类写操作的授权测试场景（取消、满背包替换、结果未知后只读核对） | NotRun |

**总体状态：真实客户端验收未完成。** 交付的是已通过本机构建、全量回归与性能对照的局部修复。

### 5. 交付物

- 逐任务提交：`31a0817`(T1) → `f7f2e02`(docs) → `c9495b5`(T2) → `3583258`(docs) → `61a98cb`(T3)
  → `9b72d7a`(T4) → `26634c3`(T5/T6) → `a9740a0`(T7) → `380ae1b`(T8) → `e2e3281`(T9 证据/docs)
  → `8511130`(T5-A) → `2297aba`(重构) → `9100f91`(T9) → `070e2de`(docs)。
- `docs/fix-perf-progress.md`（本文件，逐项状态与证据）、`docs/fix-perf-report.md`（结论/根因/风险）。
- 未执行：推送、合并、发布；未在真实账号做压力发包或额外写入。


---

## 实机反馈修复（2026-09-16，部署 `2.0.0-c28ca996b1d3-20260916T233245Z`）

### A. `实例 1728 派生准备失败：one derivation key was reused with different calculation seed`

现场数据（用户客户端缓存，只读分析）：`details/1728.json` 为 `complete:true` 且带 `r=7549` 无 `ri`；
`inventory.json` 同名实例带 `ri=7549` 无 `r`；两者 `fr`、`_metaRaceId`、`lv` 相同。

根因：`petRaceId()`（`r` 优先，否则 `ri`）视两种写法为同一族值，`calculationOverlayDiffers()` 只比较
解析后的种族，因此“列表只带 `ri` → 详情带 `r`”不会推进记录键；但 `sameCalculationSeed()` 逐个比较
原始字段 `id/r/ri`，于是同一个记录键在两次请求里被判为**不同计算种子**，`PetDerivationCache::request()`
以 `InvalidRequest` 拒绝该实例（界面提示派生准备失败），而此前按旧种子算出的养成事实仍挂在旧键上供
后续命中——比报错更隐蔽的是一份按旧输入得到的事实。

同一不变式的第二处：`PetRepository::applyDetailCache()` 对已知原文一律沿用旧键，即使重载改变了
`complete`/`sourceKnown` 或 `calculationOverlayDiffers()` 覆盖的字段，也会把新输入发布到旧版本号下。

修法（提交 `e39dc0e`、`ec1cad3`）：
1. `sameCalculationSeed()` 只比较计算实际读取的字段（resolved `raceId`、`fr`、`lv`、`_metaRaceId`、
   `metadataSlotMaxLevel`、`detailAvailable`），不再比较同一身份的别名写法；真正变化的输入（如等级）
   仍会被拒绝。
2. `applyDetailCache()` 仅在完整性与来源证据、计算相关字段投影都不变时才沿用旧键。

回归（先失败后通过）：`tests/pet_derivation_cache_smoke.cpp::raceAliasSeedEquivalence()` 构造同一记录
键的“列表形状（仅 `ri`）”与“详情形状（`r`）”，断言后者必须是 `CacheHit`；修改前
`validation-logs/fix-seed-alias-prefix.log` FAIL，修改后 `fix-seed-alias-run2.log` 通过。
全套 `validation-logs/fix-seed-alias-full-ctest.log` 72/72。

### B. `1039_3_0 / 1039_4_0 响应被拒绝或结果类型错误`

现场：只读（未核实来源）会话下刷新兑换次数，商店类命令更新成功，只有两个 `null` 扩展命令
（`1039_3_0` 回归每日任务等级配置、`1039_4_0` 回归特惠商城）被判失败；账号缓存里这两个活动
始终没有落库，与“从未被接受”一致；客户端离线数据中也没有这两个命令的真实回包样本
（`tests/fixtures`、仓库文档、客户端目录均已检索）。

判定路径：`shop_exchange_controller.cpp` 只读分支 `!successfulReply(packet)`，即响应带非空 `$`
或带 `r` 且不为 1。

本轮改动（提交 `661a00c`）：**不放宽判定**，但把原因显式化——`replyRejectionReason()` 区分
`$`/`r`/非整数/缺成功标志，`boundedPayload()` 截取 512 字符，两者同时进入 GUI 状态文本与诊断日志
（`reply rejected command=… reason=… payload=…`）。需要用户再点一次“刷新兑换次数”取回真实回包形状，
再按证据修解析；在拿到证据前按任务书不得猜测放宽成功判定（否则会把错误响应当成有效观察写库）。

### B. `1039_3_0 / 1039_4_0 响应被拒绝或结果类型错误`（已定性并修复）

真实回包（用户客户端 `C:\Users\25982\Desktop\11\氪奇Pro-V1.1.4` 的
`run-46676-2cbe824cc834f0227144a8a3b0361ac6` 诊断日志，由本轮新增的诊断输出）：
`1039_3_0响应被拒绝或结果类型错误（r=-2）`、`1039_4_0响应被拒绝或结果类型错误（r=-2）`。

根因：两者是**回归（return-player）类活动**，目录中带官方解锁门 `unlock="NewBack30Day$1"`
（`30天回归2026版tab4-每日任务`、`回归特惠商城`）。当前账号不满足该门，服务器直接以 `r=-2`
拒绝查询；旧实现把“非成功回复”一律当成解析/结果错误，于是每次刷新都报一次、并且每次都再问一次。
历史日志也印证：`run-29016`（09-13 10:04，session_generation=1，目录尚未包含这两个活动）没有该
警告，自 `run-11092`（09-13 13:43，session_generation=2）起每次刷新都固定出现两条。

修法（提交 `9471f6c`）：`serverRefusal()` 判定 `$` 为空且 `r` 为负整数即“服务器拒绝该查询”；
被拒来源按“当前不适用（服务器返回 -2）”报告、绝不写入观察、旧观察原样保留、同一会话内不再
重复查询（换会话自动恢复）；成功判定规则未放宽（`r` 不为 1 或 `$` 非空仍拒绝）。

回归：`tests/activity_shop_controller_smoke.cpp` 的拒绝场景（状态说明原因 / 旧观察不被覆盖 /
下一轮不再询问 / 其余分组照常推进），修改前 4 条失败（`validation-logs/fix-1039-refusal-prefix.log`），
修改后通过（`fix-1039-refusal-run6.log`）；全套 `fix-1039-full-ctest.log` 72/72。
部署：`2.0.0-596c356033f4-20260917T031331Z` 已同时部署到
`D:\奥奇传说\氪奇Pro-V1.1.4` 与桌面副本（`KQPetReleaseCheck --resolve` ok，原版程序未改动）。
