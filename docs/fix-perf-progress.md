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

## T3–T10

状态：`Pending`（按批次推进；批次二为 T3–T6）
