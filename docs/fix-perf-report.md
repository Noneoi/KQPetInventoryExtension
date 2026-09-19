# 修复与性能优化报告（T0–T10）

> **历史记录。**任务书 T0–T10 的结论报告（分支 `fix/kq-reliability-perf`，已合入 main）。文中的源码路径是当时的位置，现位置见 [architecture.md §8](architecture.md#8-路径迁移对照2026-09-19)。

- 分支：`fix/kq-reliability-perf`（基线 `4d29a6e409a619ae38b39f524aa78ff87aa50e35`）
- 最终提交：`070e2de`（本文件随该提交交付）
- 执行环境：Windows x64 / MSVC 19.51.36231 / Qt 6.6.3 (`D:\Qt\6.6.3\msvc2019_64`) /
  CMake 4.4.2 + Ninja / Python 3.14.7 / git 2.55
- 最终二进制标识（`build-agent-fix/bin/Release`，Release 配置）：
  - `KQPetInventory.dll` sha256 `44CB7669F36E44A76E3D5BF899ED72A2577309F733EBE740870EA2BD02804425`（5396992 字节）
  - `KQPetLauncher.exe` sha256 `A5C12F13CD0411E304A12060622BCF179D36B643F291D7724A02573EF8430541`（309760 字节）
- 逐任务状态与日志路径见 `docs/fix-perf-progress.md`；本文件给结论、根因、前后测试与风险。

## 1. 结论速览

| 任务 | 状态 | 生产代码改动 | 关键日志 |
| --- | --- | --- | --- |
| T0 基线 | 已完成 | 无 | `validation-logs/batch3-full-ctest.log` 等 |
| T1 目录身份 | 已修复（`31a0817`） | `src/loader/data_root_config.{h,cpp}` | `t1-prefix-repro.log` |
| T2 更新器编码 | 已修复（`c9495b5`） | 3 个 `tools/public_*_updater.py`、`tools/check_ui_boundary.py` | `t2-encoding-prefix-repro.log` |
| T3 玩法汇总完整性 | 已修复（`61a98cb`） | `asset_analysis_types.h`、`asset_analyzer.cpp`、`routine_overview_controller.h`、`asset_analysis_window.{h,cpp}` | `batch2-targeted-ctest.log` |
| T4 图片显示/落盘分离 | 已修复（`9b72d7a`） | `src/application/image_service.{h,cpp}` | `batch2-targeted-ctest.log` |
| T5-A 移动前置校验 | 已修复（`8511130`、`2297aba`） | `src/extension/pet_repository.{h,cpp}` | `t5a-prefix-final.log`、`t5a-move-final-run1..3.log` |
| T5-B 大量更新只发布最新 | 已验证 | 无（测试断言过时） | `t5-analysis-cache.log` |
| T6 推荐容量/预算 | 已验证 | 无 | `t6-recommendation.log` |
| T7 概览局部更新 | 已修复（`a9740a0`） | `asset_analysis_window.{h,cpp}` | `t7-ui-preview.log` |
| T8 详情突发发布 | 已验证（实测无收益，不改） | 无 | `t8-baseline-run1..3.log`、`t8-final-run1..3.log` |
| T9 同源图片复用 | 已实施（`9100f91`） | `src/application/image_service.cpp` | `t9-prefix-repro.log`、`t9-final-run1/2.log` |
| T10 冷分析专项 | 已验证（指标已读，无需改动） | 无 | `t10-performance.log`、`build-performance-results/batch3-t10`、`batch4` |
| 批次四 | 全量重建+全量回归+性能对照已完成；真实客户端验收未完成 | — | `t9-full-ctest-2.log`、`batch4-performance.log` |

## 2. 逐项：证据、根因、最小修法、修改前后测试

### T1 缓存目录迁移的身份比较

- 证据：迁移到 8.3 短名别名目标时 `_wcsicmp(committed.dataRoot, pendingRoot)` 判定不同，
  回退为“缓存迁移的目录配置尚未提交。继续使用原缓存目录”，2 条断言失败（`t1-prefix-repro.log`）。
- 根因：词法/大小写比较不能证明“同一目录”；别名拼写与真实身份无关。
- 最小修法：新增 `DirectoryIdentity{Same,Different,Unverifiable}` 与
  `compareDirectoryIdentity()`、`plainDirectoryPath()`（逐级解除 junction/别名），迁移验收改为
  “先取各自的长路径，再比较 `FileIdInfo` 或 non-zero 卷序列 + 文件索引”；无法确认身份时给出
  明确的 Windows 错误信息而不是猜测。
- 前后：修改前 2 FAIL → 修改后 `data_root_config_smoke` 8.3 别名、身份、链式长路径用例全通过。

### T2 更新器进度行编码

- 证据：42 failures + 3 errors，`UnicodeEncodeError: 'ascii' codec`、`'charmap' codec`、
  `'gbk' codec can't encode character '\U0001f680'`（`t2-encoding-prefix-repro.log`）。
- 根因：机器消费的进度行随宿主 stdout 编码变化；规则本身要求机器可解析。
- 最小修法：三个更新器的**进度行**统一 `ensure_ascii=True`（纯 ASCII JSON），文件输出仍为
  `ensure_ascii=False` UTF-8；新增 `tests/public_updater_output_encoding_test.py`（3 个发射器 ×
  4 种载荷 × 5 种流编码 × 管道/重定向）并注册为 `public_updater_output_encoding_smoke`。
- 前后：修改前 42+3 → 修改后退出码 0。

### T3 玩法剩余汇总表达来源完整性

- 证据：日常/周常剩余次数在“部分来源未更新”时仍按 0 累加，UI 显示“剩余 0 次”。
- 根因：`analyzeRoutine()` 用未区分来源状态的标量求和，且竞技场有效性键写成了从未产生的
  `16_24_A:activity`。
- 最小修法：新增 `RoutineCompleteness`/`RoutineOpportunitySummary`，按 8 个来源逐项判定
  `Value/Empty/Invalid/Missing` 与是否已录制，单遍汇总；UI 文案区分“已确认/未更新/未确认/合计无效”。
- 前后：`routine_overview_smoke` 的 Partial(45)/Complete(47,6)/竞技场拆分(6,7→54)/溢出/切号用例全通过。

### T4 图片显示与落盘结果分离

- 证据：批量条目把“已显示”计为“已保存”，写盘失败不可见。
- 根因：批量记账挂在 `completed(Ready)` 上；保存路径无显式成败语义。
- 最小修法：`ImagePersistence{Saved,AlreadyValid,Failed,Cancelled}` +
  `persistenceCompleted` 信号；批量记账改为按落盘回执；新增 `ImageIo::verify()` 与
  `validStoredImage()`；取消/占用/队列拒绝/元数据未提交等各自给出明确原因。
- 前后：`image_service_smoke` 的故障注入块（阻塞队列、锁占用、网络键元数据失败、陈旧批次回执、
  服务销毁）全部按预期给出 Failed/Cancelled 而非静默成功。

### T5-A 磁盘详情重载不应推进事实修订（本次最重要的可靠性修复）

- 证据：`pet_move_controller_smoke` 14 条确定性失败；现场 `[REV1487] id=1 knownOrig=0
  prevComplete=0 prevPersisted=0 curEmpty=0 eqRoster=1 added= changed= removed=` 与
  `[PREFLIGHT-INVALID] ... rev=56 vs 55 authoritative=1 phase=5`（`t5a-instrumented*.log`）。
- 根因：`PetRepository::applyDetailCache()`（磁盘详情读取完成路径）对**逐字节等同**的重发
  无条件 `++inventoryRevision_`；该自增落在“移动意图正在落盘”的窗口内，使
  `movePreflightStillValid()` 判定失败，用户已确认的移动被记为未提交。同类自增还出现在
  声明许可无关的原始字段（`_position`）差异上。
- 最小修法：发布前记录消费者可读的三项事实——名册副本、合并详情视图（`mergedRecordView()`，
  与 `detailFor()` 同一合并结果但不计裸导出）、记录的 `complete`/`sourceKnown`；仅当其中任一项
  确实变化（或该实例不在名册中）才自增。`2297aba` 进一步把内部比较从 `detailFor()` 拆出，
  避免把内部读取计入 `untrackedExports`。
- 前后：新增 `repository_smoke.cpp::diskDetailReloadRegression`，修改前
  `revisionAfterLists=5 → revisionAfterReload=6` 且断言失败（`t5a-prefix-final.log`），修改后
  `5 → 5` 通过（`t5a-fix-repository.log`）；`pet_move_controller_smoke` 14 → 0 条失败，
  连跑 3 次一致（`t5a-move-final-run1..3.log`）。
- 等价性论证：`inventoryRevision()` 的消费者是移动写前置、分析输入戳、发布器与快照存储，均读取
  `briefFor()`/`mergedRecordView()` 与记录键；名册副本 + 合并视图 + 完整性/来源证据均不变时，
  可见事实与分析输入没有变化，`complete`/`sourceKnown` 变化仍必然自增（`repository_smoke` 的
  “new detail facts must invalidate”断言仍通过）。

### T5-B / T6 / T8 判定为“不改生产代码”

- T5-B：断言用本地可达战力作为新版本判据，而该夹具只带一个分项，按已确认规则本地总数必须保持
  未知；改为按“输入版本 + 该版本观察值”断言，保留计算/驻留上限断言。
- T6：`reserveOutput()` 按 P 预留、每宠最多“商店行 + 本地行动行”两行、额外行按 capacity 记账、
  超预算立即取消且不发布半成品；补 4 宠 8 行 + 容量恒等式 + 小预算空结果回归，未改生产代码。
- T8：合并窗口与无操作写入守卫实测收益为 0（`publications=1`，前后一致），且守卫让时序敏感的
  压力断言 2/3 失败；按“无可测收益的复杂改动不合入”回退，仅保留回归与实测证据。

### T7 资产概览局部更新

- 证据：每次分析后整表重建（100+ 行）并重复统计四个过滤器计数。
- 最小修法：按分析版本缓存 4 个分类计数（失效点在 `applyAnalysis`/`refreshAccountAnalysis`/
  `resetSessionContext`），用 label→row 映射原地更新行，例行更新走 `updateRoutineOverviewRows()`
  不再 `setRowCount(0)`；新增 `overviewPetScanCount()` 供回归断言扫描次数。
- 前后：`asset_analysis_ui_preview` 自检块断言“第三次分析只多扫描一次”、行数清空/恢复、例行局部更新；
  `t7-ui-preview.log` 通过，全套回归通过。

### T9 同源图片任务复用

- 证据：同源两尺寸 `downloads=2`（修改前 `t9-duplicate.log`/`t9-prefix-repro.log`）。
- 根因：请求键包含输出尺寸，`active`/RAM 缓存按请求键区分，同源的读取/下载/导出阶段没有复用。
- 最小修法：`SourceTask` 在途源任务表（资源身份 + 资源版本，不含尺寸/DPR），消费者引用计数决定
  取消，显式刷新/目录重载/关闭各自隔离，每源只有一个写盘者，其余消费者在其回执后确认磁盘；
  解码失败按候选游标重建源并把其它消费者一并退回等待。
- 前后：断言收紧为 `downloads == 1` 且两个尺寸各有 `Saved/AlreadyValid` 回执；新增“在途加入 +
  批次取消”夹具（`/hold-join`）。修改前 `2 / 2`、修改后 `1 / 1`，全套 72/72。

### T10 冷分析指标读取

- 数据：见 `docs/fix-perf-progress.md` 的 T10 表格（p2000/p10000 × warm/cold 的阶段指标）。
- 结论：无重复解析（读文件数/派生次数与精灵数 1:1）、无失效任务残留（`InputRejected` 出现在
  preparation 且属 facts 账本预算判定）、单宠派生成本跨规模一致（578 µs vs 591 µs）；冷分析时间
  由读队列等待（~11 ms/次）与派生（~0.58 ms/宠）两项线性构成。唯一“缩短墙钟”的手段是提高并发/
  预算或替换算法，前者为任务书禁止的做法，后者缺少等价性证明与热点证据，故不改动。

## 3. 测试与验收

- 全量回归（最终提交、`build-agent-fix`，Release）：`validation-logs/t9-full-ctest-2.log`
  → **72/72 通过**；最终提交复跑 `validation-logs/batch4-final-ctest.log` → 70/72，两个失败均为
  下述**本机时序相关旧用例**（仅文档提交使二进制与 `t9-full-ctest-2.log` 完全一致）。
- 新增测试注册：`CMakeLists.txt` 中 `public_updater_output_encoding_smoke`（Python）、
  `data_root_config_smoke`、`repository_smoke`（新增 `diskDetailReloadRegression`）、
  `image_service_smoke`（T9 两条新断言）、`asset_analysis_ui_preview`（T7 自检）、
  `recommendation_smoke`（T6 容量/预算）、`routine_overview_smoke`（T3 完整性）。
- 仍失败的旧用例（按任务书不得遗漏，均已核实与本批改动无关）：
  1. `analysis_cache_integration_smoke` 的读压力场景（515 精灵、单记录 raw 缓存、256 深读队列、
     20s 预算）在本机时会超出预算：`t9-analysis-probe-instrumented.log` 显示
     `running=1 protected=1 resident=1 evicted=286 refused=2150 retries=2543`。把
     `pet_repository.cpp` 回退到 `e2e3281`（无 T5-A 修改）后同样 3/3 失败
     （`t9-prefixT5a-analysis1..3.log`）；该用例不使用 ImageService（T9 只改图片服务）。
     同一二进制早前运行 13–21s 通过。
  2. `shop_ui_preview_smoke` 在 ctest 的 15s 超时下超时且 stdout 无输出：直接运行同一二进制
     8.3s、退出码 0，自检打印 `SWITCH: first_call_ms=0 heartbeat_pulses=2982
     membership_cache_hits=6 requests=0`；把 stdout 接管道后同一次运行变成 **17.8s**（超过 15s），
     即该用例耗时对“stdout 是否被管道消费”与本机速度敏感，而 ctest 恒为管道
     （`batch4-shop-preview-run1..3.log`、`batch4-shop-preview-verbose.log`）。
- 判定：本机当前比早前全量运行慢约 1.4–1.7×（同二进制 8.3s→17.8s、13s→24s），这两个时间预算
  边界用例因此落到预算之外；`t9-full-ctest-2.log` 的 72/72 表明代码路径本身可全绿。

## 4. 性能对照（同条件、同参数：`-Matrix Priority -Warmup 1 -Samples 3`）

原始样本：`build-performance-results/t0-baseline`（基线）、`batch3-t10`、`batch4`（最终提交）。
下表为 p50 阶段指标（未标注单位均为纳秒原值换算）。

| 用例 | 指标 | T0 基线 | 批次三 | 批次四（最终） |
| --- | --- | --- | --- | --- |
| p2000-g200 warm | preparationToFrozenInputNs | 43.4 ms | 31.8 ms | 23.6 ms |
| p2000-g200 warm | candidateComputeNs | 69.1 ms | 53.0 ms | 38.0 ms |
| p2000-g200 warm | snapshotSaveNs | 39.7 ms | 28.0 ms | 23.1 ms |
| p2000-g200 warm | sampleThroughPersistenceNs | 209.1 ms | 144.8 ms | 114.0 ms |
| p2000-g200 cold | preparationToFrozenInputNs | 7.07 s | 5.80 s | 5.16 s |
| p2000-g200 cold | cacheRawDeriveActiveWallNs | 1.36 s | 1.16 s | 0.99 s |
| p10000-g1000 warm | preparationToFrozenInputNs | 22.9 s | 21.5 s | 18.1 s |
| p10000-g1000 warm | cacheRawDeriveActiveWallNs | 4.57 s | 4.24 s | 3.64 s |
| p10000-g1000 warm | sampleThroughPersistenceNs | 24.5 s | 23.7 s | 19.4 s |
| p10000-g1000 cold | preparationToFrozenInputNs | 23.3 s | 24.1 s | 19.2 s |

- **输出正确性**：p2000 两组 `Published`（正确性断言全部通过），p10000 两组仍为 `InputRejected`
  （facts 账本超预算，与 T0 基线相同，属预算/工作集判定）；批次四与基线**没有出现新的失败类别**。
- **内存账本**：`privateBytesSampledDelta` p2000 cold 114.9 MB（批次三）→ 批次四无新增常量；
  `privateBytesIncrementFromBeforeDataset` p10000 404 MB（批次三）——两组均为既有工作集特征，
  本批改动未扩大任何预算常量。
- **收益归因的诚实说明**：阶段指标是墙钟量，受机器负载影响；批次三与批次四的差异（约 −20%）
  与本批改动的目标（UI 局部更新、图片复用、移动前置修复）并不构成直接的因果链，因此**不作为
  已证实收益声明**；可确认的是“同条件复跑无退化、输出结论不变、失败类别不变”。

## 5. 未复现 / 阻塞项

1. **真实客户端验收未完成**（10 项全部 `NotRun`）：本机无客户端访问与操作授权，按任务书不自行
   发送游戏写请求。待验项目与判定口径见 `docs/fix-perf-progress.md` 批次四小节。
2. `analysis_cache_integration_smoke` 读压力场景的预存在时序敏感失败（见第 3 节）。
3. T9 未测量多消费者并发峰值与 swf 提取子进程启动次数分布。
4. 图片保存“提交前取消窗口”的确定性注入仍未做到（当前仅能覆盖 I/O 队列拒绝、锁占用与队列满）。
5. 跨天/跨周真实周期翻转、多实例/多账号并发写盘未做真机验证。
6. 批次四未执行推送/合并/发布，也未在真实账号上做压力发包或额外写入。

## 6. 兼容性与风险

- 改动均落在既有接口内，未变更公开协议字段、未新增第三方依赖、未改动构建配置；C++ 侧新增符号
  均为内部实现（`DirectoryIdentity`、`ImagePersistence`、`RoutineCompleteness`、`SourceTask` 等），
  对既有调用方保持向后兼容。
- 语义收紧项：图片“显示”不再等于“已保存”（批量条目按落盘回执记账）；玩法剩余次数在来源不完整时
  显示“未更新/未确认”而不是 0；移动写前置在事实未变化时不再因无关自增而中止。
- 风险与缓解：T9 共享源引入消费者引用计数与每源单写者，已用“在途加入 + 批次取消”“同源两尺寸
  回执”两条回归与既有故障注入夹具覆盖；T5-A 放宽的是“无可见变化时的自增”，`complete`/`sourceKnown`
  变化、名册变化、非名册实例仍必然自增，`repository_smoke` 的既有不变式断言保持通过。
