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
| `.\scripts\run-performance.ps1 -Executable .\build-agent-fix\bin\Release\KQAssetAnalysisPerformance.exe -Matrix Priority -Warmup 1 -Samples 3` | 2 | `validation-logs\t0-performance-baseline.log`、`build-performance-results\t0-baseline\` |

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

状态：`Pending`

已确认的失败面（基线 ctest）：`ui_boundary`、`protocol_fixture_smoke`、`recommendation_smoke`。

编码线索（待实施）：`tools/public_data_updater.py:emit()`、`tools/public_activity_exchange_updater.py:emit()`、
`tools/public_routine_updater.py:_emit()` 使用 `ensure_ascii=False` 写 stdout；
Qt 调用方（`src/application/data_update_service.cpp`）与 `tools/bootstrap-public-data.ps1`
都显式设置 `PYTHONUTF8/PYTHONIOENCODING=utf-8` 并继承给子进程，因此**应用内**调用当前可用；
独立调用（脚本直跑、无该环境变量）在 cp936/cp1252 控制台下会写出非 UTF-8 字节或直接抛
`UnicodeEncodeError`，使消费者解析不到 `progress`/`finished` 事件。

---

## T3–T10

状态：`Pending`（按批次推进）
