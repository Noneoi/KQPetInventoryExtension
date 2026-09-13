# v2 性能基准工具与结果解释

本工具实现 `v2.0-reconstruction-plan.md` 第 12 节的可重复采样入口。它在隔离进程运行真实 `AssetAnalysisController → PetRepository / RawCache / PetDerivationCache → AnalysisWorker`，结果提交真实 `AssetAnalysisModel` / `AssetAnalysisFilterProxyModel`，固定 100% DPI 绘制 1100×650 的可见表格视口。Core、Storage I/O、Compute 与 GUI 分离。没有 Bridge 或网络发送能力，不读取真实账号目录。离线假 Sender 仅在夹具准备时创建生产商店解析器要求的请求预期；准备和计时内计数分别报告，计时内必须为 0。

**2026-09-12 用户调整：预算作为参考，允许合理余量。**日常验证优先完整数据、主要流程和实际使用体验，不因超出原来的耗时或内存数字卡住重构。默认快速抽测；`-Strict` 仅供主动选择的详细对照，不是发布前置要求。历史测量原样保留。

最新单轮完整验证已通过：10,000×1,000 稀疏、100% 详情、8 KiB 用例发布 10,000 行、H=50,000，并成功保存快照。记录为 `build-v2-performance-frozen/production10000-sparse-advisory-final.jsonl`。本轮采用结果 128 MiB、Worker 总账本 512 MiB、Storage 队列 128 MiB，以及可取消的 120 s 准备超时；没有降低 P/G/H 或详情大小。冷点击约 38.27 s、保存约 116.6 ms，仅作参考，正常退出且全部正确性检查通过。

## 运行方式

```powershell
.\scripts\build.ps1 -BuildDirectory .\build-v2-performance -Targets KQAssetAnalysisPerformance

# 优先测 2,000×200、8 KiB、常规 10 候选，以及 10,000×1,000、稀疏 5 候选。
.\scripts\run-performance.ps1 `
  -Executable .\build-v2-performance\bin\Release\KQAssetAnalysisPerformance.exe `
  -Matrix Priority

# 全部声明组合；该命令会做大量计算和临时详情文件读写。
.\scripts\run-performance.ps1 `
  -Executable .\build-v2-performance\bin\Release\KQAssetAnalysisPerformance.exe `
  -Matrix Full

# 独立挑选一个完整矩阵 case，按需要增加采样。
.\scripts\run-performance.ps1 `
  -Executable .\build-v2-performance\bin\Release\KQAssetAnalysisPerformance.exe `
  -CaseId p2000-g200-normal-d100-k8-c3-m3 -Warmup 5 -Samples 30
```

运行脚本输出到新的 `build-performance-results/<时间>/`，保留已有证据目录。默认每个热进程 **1 次预热＋3 次测量**，可用 `-Warmup`、`-Samples` 调整；显式 `-Strict` 且未指定次数时才使用 5＋30。冷组使用新进程、0 次预热及 1 次测量，`-ColdRuns` 可增加次数。不会清理操作系统文件缓存，因此名称是“应用冷缓存”。标准进程超时为 600 秒，可通过 `-RunTimeoutSeconds` 调整。

CLI 可用于开发时检查少量数据。例如：

```powershell
$env:PATH = 'D:\Qt\6.6.3\msvc2019_64\bin;' + $env:PATH
.\build-v2-performance\bin\Release\KQAssetAnalysisPerformance.exe --list-cases
.\build-v2-performance\bin\Release\KQAssetAnalysisPerformance.exe `
  --pets 64 --goods 20 --match sparse --detail-percent 100 --detail-kib 8 `
  --cost-items 3 --cultivation-items 3 --warmup 0 --samples 1 --cache warm
```

开发用小 case、少样本或 `--no-phase-probes` 仅用于工具正确性检查；正式报告不能用它们替换完整规模。无参数的 CTest 仍执行原有宽松 smoke，检查 1,000/2,000 行、脏标记去重、不自动重算、Model 更新与快照保存；不会在常规构建时自动启动全矩阵。

## 数据集

种子默认固定为 **20260909**。矩阵含 2,000×1、2,000×实际在线目录、1,000/2,000/10,000×200、2,000×1,000、10,000×1,000；覆盖 zero、sparse（每宠 5 候选）、normal（每宠 10 候选）、full，详情完整率 0%/50%/100%，以及完整详情约 8/32 KiB。合成商品独立交叉 1/3/10 个成本项与 1/3/10 个培养项。

合成目录用循环种族关联索引产生 K 个候选，实际保留每个 `raceIds`，不会预先生成 P×G 候选表。半数个体通过当前种族匹配，半数通过元数据种族匹配。完整详情包含已知培养分项、星神序列/背包、元魂、神源兽、星轮、装备、天赋，以及 `sppl`、`cppl` 关系个体。目标字节数由真实 `sgsp` 星神背包条目扩展达到；生产战力函数实际遍历并排序这些条目，未增加无关填充字符串。该数据模拟字段形态与压力，不宣称星神库存数量是线上典型分布。

每行 dataset 会报告 UTF-8 **实际**总字节数、完整详情均值/最小/最大字节数、完整详情个数、G、A、每宠候选范围，以及成本/培养项个数分布。没有完整详情的个体保留摘要，0% 组不会伪造 8 KiB 已有详情。

实际目录固定业务日期 **2026-09-09**，保留其真实规则和上下架条件；在线商品数由目录读取。实际种族不能凭空匹配全部商品，所以各密度参数选取最接近目标候选数的真实种族，并报告实际 H，不把 `full` 字样当成真实全笛卡尔匹配的证据。实际目录不修改成本/培养项，因此这部分不重复套入人为的 1/3/10 规则。完整矩阵共 **1320 个 case**。

账号材料余额和已用次数通过真实 `ShopExchangeController` 的已验证合成 envelope 入口接收；每个有限次数字段使用真实 `acceptQuotaValidityEvidence`，绑定 account、epoch、group、observationSequence 和固定 UTC 时间范围，并报告 synthetic 来源。`ObservationClock` 固定在 2026-09-09 10:00 UTC。实际目录中的解锁表达式没有被补造事实，因此缺事实的解锁仍按生产规则为 Unknown。`AnalysisEnvironment` 为业务日期、目录和元数据提供冻结依赖，版本戳、缓存键、准备及最终捕获使用同一来源。

目前已实现的培养类型只有 9 种，10 项边界包含第十个独立未知代码 `999`，按生产规则保留 Unknown；不通过重复同一代码冒充 10 个被处理的项。`tenComponentBoundaryIncludesUnknownRule` 在报告中明确标注。

`HByRace` 是所有实例与商品的种族潜在关联数；`HExpectedWithCompleteDetails` 是具有完整详情、可进入生产资格计算的关联数。生产实访 H 在 `pipeline.candidatePairsVisited` 中报告。0% 详情下生产算法跳过无法派生的个体，不能据此声称已经计算过完整的潜在 H。已发布结果检查实际访问数与可判断 H 相符，且完整保留 P 条个体结果。

## 时间与线程测量

计时使用单调时钟，单位为纳秒。夹具生成和观察入口在计时外执行：最多 12 只背包按指定完整率作为真实观察包进入 Repository，由生产 Storage 保存；仓库仅提交摘要，将完整原文以 schema 3 写入该隔离账号的 `details/`。Repository 初始目录扫描先完成，避免在点击前读入仓库正文。没有预先派生 facts，也没有另一套捕获器或 Worker。

“点击”开始于 GUI 向 Core 投递命令之前，第一次点击才把空派生缓存连接到 Controller。真实 Controller 完成磁盘补知、Core 解析、缓存任务、共享 Compute 派生、冻结 compact facts，再交同一个生产 Worker 计量、编译、资格/排名和排序。GUI 提交和实际绘制后记录 `clickToVisibleNs`。后续暖轮保留生产缓存和持久索引，缓存命中、实际 raw factory 次数、Worker 复用 facts 次数分别报告。

应用冷缓存要求新进程与空派生 RAM/disk。已经观察到的背包 raw 数和计费字节在 dataset 中单列，因此没有把这类真实现有观察伪称为零 raw。0% 组不会固定补全 12 只。完整率、P/G 和原始字节均不缩减。该入口覆盖真实生产控制器及缓存的端到端路径，不包含硬件按钮输入、网络等待或工作台切页。

| 字段 | 含义 |
|---|---|
| `preparationToFrozenInputNs` | Core 接收点击至 `analysisInputCaptured`，含必要磁盘读取、排队、Core 解析、缓存派生与最终冻结 |
| `readBytes` / `readFiles` / `missingOriginalReadFiles` | 生产 Storage 实际完成的原文读取字节、成功数与 NotFound 数；不由 harness 重新解析正文 |
| `rawDiskReadActiveWallNs` / `rawDiskReadQueueWaitNs` | Storage 实际路径检查/open/stat/read/close 回调累计耗时（不含 hash），及各请求从入队至 IO 分派的累计等待；等待区间可以重叠，不能与端到端时间直接相加 |
| `coreRawJsonDecodeNs` / `coreRawApplyNs` | Repository 正式原文 JSON 解析，以及验证、RawCache admission 和同步 Core 回调累计耗时 |
| `cacheComputeActiveWallNs` | 生产缓存 executor 回调内累计 wall spans，排除排队/磁盘等待；含实际取消/替代任务和索引校验，可能含 OS 抢占，不是线程 CPU tick |
| `cacheRawDeriveActiveWallNs` / `cacheIndexDecodeActiveWallNs` | 上一项内 Domain factory 和持久索引 JSON/codec/checksum 的实际子区间；不与总回调值再次相加 |
| `cacheComputations` / `cacheHits` / `derivedIndexHits` | 样本内实际 raw factory、RAM 命中及持久索引命中计数 |
| `workerQueueMeterPrepareCalculateNs` | 冻结输入完成至生产 Finished，包含输入计量、目录准备、资格/排名和排序；Controller 已在此前排队，因此不声称覆盖此前的队列时间 |
| `activeInputMeterNs` / `catalogCompileNs` / `conditionPrepareNs` | 生产 InputMeter 活跃区间、冷材料名表/目录编译/种族索引，以及每次独立账号条件准备；缓存命中时目录编译为 0 |
| `candidateComputeNs` | RecommendationSession 实际 step 活跃时间减去最终 sort，含 facts 校验、逐宠准备/收尾、H 候选和结果计量；不含排队等待 |
| `productionComputeActiveWallNs` | 同一个 Compute 执行器上缓存回调活跃时间，加实际 InputMeter、编译、账号条件、候选及排序区间；稀疏 5 s 门槛检查此项，不能只检查已经移走派生工作的 Worker 尾段 |
| `sortNs` | 生产最终排序的实际累计时间 |
| `guiModelCommitNs` / `guiPaintNs` | GUI Model 提交与可见视口绘制 |
| `guiSearchFilterSortNs` | 真实资产 Proxy 的筛选、搜索、排序；150 ms 防抖单列、不混入计算 |
| `snapshotAdmissionNs` / `snapshotSaveNs` | 通过 Controller 录制入口的构造/入队及 Storage 完成原子保存区间；队列拒绝保留为失败 |
| `sampleThroughPersistenceNs` | 点击至必要缓存索引、快照保存和可选独立 probes 全部结束；不是结果可见时间 |
| `maximum*SliceNs` / `maximumAtomic*Ns` | 生产不可抢占片段和工作片的最大耗时 |
| `coreEventLoopMaximumGapNs` / `guiEventLoopMaximumGapNs` | 5 ms 周期探针观测到的最大事件间隔，含名义周期，属于保守响应指标 |
| `computePriorityMaximumWaitNs` | Core 每 5 ms 尝试一个有界高优先级空任务，在现有 Compute 执行器上测实际排队等待；最多一个在途探针 |

`independentPhaseProbes` 在同一 Compute 执行器上独立调用生产目录编译和账号条件 API。它们在结果可见和保存之后执行，**不与端到端耗时相加**。不在计时后重建另一份全量原文来测假派生阶段。各生产阶段由实际代码段累计值观测；候选阶段包含其必要的 facts 校验/准备/结果计量，不把这个区间说成只有 H 判断。`goodsCompiled`、`goodsReused` 和 `compiledCatalogCacheHits` 分别报告实际编译与复用，每次账号条件仍重新准备。

取消通过真实 `AssetAnalysisController::cancelAnalysis()` 触发，使用 `--cancel-phase preparation|worker` 分别覆盖准备阶段与输入捕获后的 Worker。按真实 Finished 的 key 和 phase 区分目标任务；只有实际发出取消并以 `Cancelled` 完成才算覆盖。`cancellationEndToEndNs` 从调用前计至匹配 Finished，Worker 内 token 延迟另列。预算先拒绝、排队取消或计算提前结束都不能冒充目标阶段通过。脚本额外跑 10,000×1,000 稀疏准备取消及全匹配 Worker 取消。普通关闭不终止工作线程；超时记录 `shutdownClean=false`。

## 内存、错误与报告

按用户最新要求预留余量：Input 128 MiB、单结果 128 MiB、候选 1 MiB、Worker 总计 512 MiB。原有 64/256 MiB 仅作为历史对照。仍保留资源上限并如实报告 `BudgetExceeded`；调整预算时记录新值，完整保留原数据规模与结果，避免靠少算数据制造达标。

`logicalMemory` 是生产 Worker 输入、结果、候选和发布重叠账本。`productionCaches` / `productionCachesAfterPersistence` 同时列出真实 RawCache、事实 resident/retained/reserved、排队输入、元数据、索引意图和 Storage 队列；每 5 ms 及完成边界采样各高水位。跨账本和可能对同一共享 payload 保守重复计费，所以 `conservativeConcurrentLedgerSumPeakSampled` 是账本总和上界，不是物理内存，不能冒充准确分配量。`Lifetime` 是进程高水位，不是本样本增量。Input 排队时可能预留 128 MiB，实际计量另见 `measuredInputBytes` 和 `inputMeasurementComplete`。GUI Model 保留上一结果直至新结果提交。

进程 Private Bytes 在 GUI 事件循环每 5 ms 采样。报告给出每次基线、到可见时的采样峰值、保存/独立 probes 后的总采样峰值，以及从夹具准备前基线计算的增量；源码事实缓存、Qt、测试视口等都可能贡献该增量。采样峰值可能漏掉短暂尖峰，不能声称是精确瞬时峰值，更不能把逻辑像素/对象估算当作整个进程内存。

输出文件：

- `run.json`：机器、CPU/内存、Qt 路径、二进制 SHA-256/大小、采样策略、源码摘要及完整性结果。
- `matrix.jsonl`：全部声明 case，防止报告选择性遗漏规模。
- 每进程 `.jsonl` / `.stderr.log`：dataset、全部预热/测量 sample、异常和完成记录。
- `executions.json`：每个真实进程的 PID、开始/结束、退出码、超时和观测到的编译进程。
- `all-results.jsonl`、`summary.json`：合并原始结果及排除预热后的 P50/P95/最大值。

默认验证完整结果、所选样本、H/候选空间及保存完成，耗时只作报告。主动使用 `-Strict` 才额外检查旧耗时参考值、搜索、GUI 阻塞和取消目标。正式发行可使用记录实际表现的 `performanceReviewed=true`、`performanceBudgetPolicy="advisory"` 与非空 `performanceNotes`，无需满足旧数字；这些记录仍须绑定实际产物。

发布工具仍需将经过统一构建的测试程序与源码清单、DLL/加载器一起绑定；此脚本的 EXE 哈希证明测了哪个二进制，不能独自证明任意当前源码就是它的构建来源。

## 历史工具检查（旧输入路径，不能证明当前生产端到端通过）

独立 `build-v2-performance` 已完成以下检查：

- 矩阵枚举 1320 条，case ID 全部唯一；原无参数 `asset_analysis_performance` CTest 通过。
- 64×20、32 KiB、50% 完整详情、10 成本/10 培养项：实际均值 32768.0625 B，32 份完整详情，目标/实访 H 均为 640，64 行发布与真实快照保存成功。
- 64×实际目录、8 KiB：在线 G=14，实际均值 8192.28125 B，目标/实访 H 均为 640，发布与保存成功。
- 1,000×200 全匹配：在 Worker 提交后 25 ms 发出真实取消，收到 `Cancelled`；单次取消响应为 48,300 ns。单次数据不能替代取消分布或严格机器门槛。
- 脚本同时运行成功的 2,000 行零匹配/0% 详情组和失败的完整 2,000×200 组，两组都保留 5 次预热、30 次测量与独立冷进程；失败组未被过滤，脚本最终返回 2。证据在 `build-v2-performance/final-tool-check-184356/`。

完整优先规模的单次冷进程结构检查发现当前生产限制未通过：2,000×200 常规组实际读取 16,387,750 B、目标 H=20,000；10,000×1,000 稀疏组读取 81,940,727 B、目标 H=50,000。两组均在逻辑输入计量达到约 128 MiB 时返回 `BudgetExceeded`，且 `inputMeasurementComplete=false`；这不是已完成的 H 或成功端到端耗时。原始记录在 `build-v2-performance/validation-*.jsonl`。未提高生产预算，也未缩短详情或减少 P/G 来把它们改报通过。

严格全矩阵仍待生产缓存/输入工作集收敛及统一源码冻结后执行。

## 当前生产控制器路径检查（2026-09-12，非严格验收）

`build-v2-performance/production64.jsonl` 使用真实 Controller/Repository/缓存链路：首轮 64 只、H=320，点击内读 52 份仓库原文、计算 64 份 facts；第二轮读文件和实际 raw 计算均为 0，仍访问 H=320 并发布全部 64 行，两轮快照保存成功。dataset 明确记录首次点击前派生 RAM 条目和计算数均为 0。另有 `production64-mixed32.jsonl`（50% 完整、32 KiB、10 成本/10 培养边界）和 `production64-actual.jsonl`（冻结实际目录）的结构检查。这些少样本结果只证明工具路径与计数，不能证明完整规模性能。

完整规模首轮失败证据保留为：

- `production2000-normal-cold.jsonl`：2,000×200、8 KiB、目标 H=20,000；生产只完成 1,003 份仓库读取，访问 H=10,150，却发布 2,000 行，其中部分被错误降为 Unknown；工具 `correct=false`。IO 队列达到预算，快照入队也失败。定位到 Controller 把暂时入队失败当成已检查的缺失原文。
- `production10000-sparse-cold.jsonl`：10,000×1,000、8 KiB、目标 H=50,000；同一入队问题导致只读 6,157 份仓库原文。输入实际计量 71,290,728 B 并完整通过；结果计费达到 67,110,170 B 后由 64 MiB 结果预算拒绝，`BudgetExceeded`，没有结果可见时间。

这些文件记录的是发现问题时的二进制，后续修复复跑使用新的文件名，保留失败历史。新工具已经接入生产磁盘、JSON、派生、编译和候选活跃区间；尚未完成统一源码冻结后的全矩阵、5＋30 严格采样。结果内存拒绝输出 `resultRetainedByteBreakdown`，区分推荐/总览的 heap 与 capacity；首次超限值不是完整结果最终所需内存。

### 独立完整重编后的生产复测

生产统计接口布局冻结后，从零构建 `build-v2-performance-frozen`，小用例和完整规模均恢复正常运行。中途混合构建曾出现的进程堆损坏未在这份完整重编产物复现；后续必须继续绑定源码与二进制，不能用该现象为其他崩溃作通用解释。

`build-v2-performance-frozen/controller-sampling-2k/` 保存了 **5 次预热＋30 次热测＋1 次独立冷测** 的完整脚本证据；本次未启用 `-Strict`，所有 36 个样本均完整发布 2,000 行、H=20,000，快照保存成功，源摘要与二进制哈希在该次脚本期间保持一致。

| 2,000×200 常规 8 KiB | P50 | P95 | 最大值 |
|---|---:|---:|---:|
| 热点击至可见 | 92.81 ms | 131.19 ms | 150.72 ms |
| 热真实 Compute 活跃总和 | 55.41 ms | 67.19 ms | 81.34 ms |

热测的原文读取、Core JSON 解码和 raw factory 均为 0，目录编译为 0、`goodsReused=200`，账号条件每次仍准备 200 项。独立冷样本读取全部 1,988 份仓库原文、派生 2,000 份 facts；点击至可见 **4.4245 s**、真实 Compute 活跃总和 **2.5742 s**，仍超出冷路径 2 s 目标。这份开发期分布不能代替最终空闲机器和完整矩阵严格验收。

`build-v2-performance-frozen/production10000-sparse-cold.jsonl` 读取全部 **9,988** 份仓库原文、派生 **10,000** 份 facts，完整输入计量 **84,464,424 B**。结果仍在 **64 MiB** 上限拒绝：首次拒绝点的总览 capacity/heap 为 2,097,024 / 32,206,174 B，推荐 capacity/heap 为 4,193,920 / 28,620,832 B。该值不是完整 10,000 行最终结果大小；没有把预算拒绝记录成成功耗时。准备约 21.10 s，其中缓存 Compute 活跃累计约 12.24 s；不能用其后 Worker 尾段约 233 ms 声称稀疏计算目标已通过。

完整 10,000×1,000 的取消检查也保留独立证据：`cancel10000-worker.jsonl` 全匹配组在 Worker 阶段实际取消，端到端确认 6.5862 ms；`cancel10000-preparation.jsonl` 稀疏组在准备阶段实际取消，35,600 ns。均收到匹配任务 key/phase 的 `Cancelled`，未减少声明的数据规模；它们仍是单次开发检查。

### 按余量策略收口

恢复后的工作确认中断前尚未实施新的星神解析/排序或 IO 路径优化，因此没有保留未经验证的半成品改动。Domain 现有规则及未知、溢出校验保持。C++ CLI 默认同步为 1 次预热＋3 次测量；脚本非 Strict 时记录源码变动，仅实际二进制和数据验证失败影响通过状态，不因开发期间其他源码编辑误报本次数据失败。

完整 10,000 只首次按新结果预算发布成功后，旧 Storage 32 MiB 保守 JSON 工作集门槛曾拒绝保存；提高到 128 MiB 后又触及旧 30 s 准备超时。这两份历史记录保留为 `production10000-sparse-advisory.jsonl` 和 `production10000-sparse-advisory-save.jsonl`。最终按 120 s 可取消准备余量运行的 `production10000-sparse-advisory-final.jsonl` 明确报告：`Published`、`correct=true`、10,000 结果及 GUI 行、0 缺详情、9,988 份仓库原文读取、H=50,000、`snapshotSaved=true`、`shutdownClean=true`、`allSamplesCorrect=true`。其余 12 只为夹具已观察的完整背包，口径与此前一致。
