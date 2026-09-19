# AI 工程交接：v2 工作台整合

核对日期：2026-09-12；2026-09-19 按代码目录整理更新路径与收尾状态。工程目录以实际克隆位置为准（早期记录中的 D:\奥奇工程\KQPetInventoryExtension 是当时的开发机路径）。日常客户端目录不是默认测试写入目标；预览发布不意味着已经覆盖日常部署。

## 先确认当前状态

这是扩展原版客户端的工程，不是 D:\奥奇工程\逆向\逆向 中的复刻登录器。根入口是稳定 Bootstrap，版本加载器和 DLL 成对位于 KQPetRuntime\releases\{releaseId}，由 manifest 与 active 记录确定加载权限。

v2 是重构任务名称。不要在文档、压缩包名或验收报告中手写一个“已发行 v2”版本号。CMake 的项目版本进入生成资源，但对某一份已有产物，身份必须从 KQPetReleaseCheck.exe --identity 读取；当前 CMake 文字不能替代旧二进制的资源身份。

**先读取当前产物随包测试和验收报告，再判断其验证状态。** 已有单项通过记录只能证明对应构建与场景；历史整合记录不替代当前配对报告，不要将它们汇总成“全测通过”或“已经部署”。

阅读顺序：

1. [README](../README.md)：当前构建、打包、部署、数据与回退命令。
2. [架构与代码组织](architecture.md)：源码分层、依赖边界、构建片段、测试布局，以及确认整理不改变功能的方法。
3. [重构计划](v2.0-reconstruction-plan.md) 与 [实施记录](v2.0-implementation-log.md)：约定、用户修正和进度（历史记录，源码路径按 architecture.md §8 对照）。
4. CMakeLists.txt 与 cmake/*.cmake、scripts/build.ps1、scripts/test-release.ps1：实际目标图及构建身份。
5. src/application/runtime/application_runtime.*、src/extension/extension_context.*：Core/GUI 装配与关闭流程。
6. 对应 Domain、Storage、Controller、ReadView、模型及测试。
7. profiles/targets.json、profiles/legacy-baselines.json 和 [逆向依据](设计与逆向依据.md)：兼容与旧版证据。

用户最新要求优先于旧计划数字：**预算应有余量，性能目标作参考，不机械卡旧门槛。** 当前默认 result 128 MiB、Worker total 512 MiB、input 128 MiB、compiled cache 16 MiB、I/O outstanding 128 MiB、分析准备超时 120 秒。常规性能抽测默认预热 1 次、采样 3 次；完整矩阵和 -Strict 是按需选项。不要继续为旧 64/256 MiB 或全矩阵强制要求扩大任务。

## 实际边界与入口

| 职责 | 主要源码/目标 | 接手时关注 |
|---|---|---|
| Compatibility | src/compatibility、Profile 生成器、CompatibilityCheck | Qt 调用前先验证 PE、架构、模块、Profile、入口策略 |
| Protocol / Bridge | src/protocol（packet_contract、session_context、inbound/outbound queue）、src/bridge（original_bridge、inline_hook） | 捕获值与来源证据；调用原版不能被业务异常或队列阻塞 |
| Domain | src/domain、KQPetDomain | QtCore 值与纯规则；不依赖单例、Repository、I/O、UI 或隐式业务时钟 |
| Storage | src/storage、KQPetStorage | 冻结写入上下文、单 I/O 线程、队列计费、锁与原子文件提交 |
| Application | src/application/{runtime,catalog,pet,shop,routine,analysis,views,images}、ApplicationRuntime | 固定 Core/Compute 调度、取消、版本校验、缓存协调与发布 |
| UI | src/ui/{workbench,pet,detail,shop,routine,analysis,common}：工作台、各页、模型、渲染 | 显式冻结元数据/事实；增量角色更新；不从 GUI 读个人目录或重算培养 |

六边界是职责关系，不要求恰好六个库。KQPetCore 是 ApplicationCore 的兼容聚合别名，不能把它误称为纯 Domain。还要检查 UI 是否仍有 Catalog/旧详情适配器反向依赖；单纯更名 target 不算完成拆分。

tools/build/check_domain_boundary.py 检查 Domain 和三个明确审查的值契约：pet_detail_types.h、pet_derivation_types.h、pet_record_types.h。该白名单不开放整个 contracts 或 Application 目录，契约内容与递归直接依赖仍需检查。QElapsedTimer 用于计量/分片；业务日期必须作为冻结输入传入。Domain 的 QtCore-only /WHOLEARCHIVE 测试补充静态检查。

## 线程、版本与所有权

ApplicationRuntime 在 GUI 管理生命周期，Core 线程拥有 Repository、Controller 和调度状态；Compute 执行纯派生与分析，I/O 执行磁盘、编码、摘要和图片网络任务。GUI 通过 InventoryProjection、AnalysisProjection 与冻结 DTO 读取结果。新任务复用现有执行器，不随每次查询临时创建线程。

账号、会话 epoch、详情 memory revision、元数据 revision/digest、分析版本和任务 generation 各有含义，不能互相替代。冻结对象发布后不再原地修改。账号切换必须立即清除个人选择、详情和旧移动目标；公开搜索偏好可以保留。

RawPetRecordHandle 持有原文及预算 lease；已知完整事实、驻留状态、Saved 状态分别记录。列表只保摘要，不能在两个 inventory map 中再存完整 JSON。只将 GUI 当前选择需要的原文送往页面，不能广播整个 raw LRU 的 handles 导致永远无法逐出。

PetDerivationCache 产生紧凑 PetAnalysisFacts；计算输入固定账号、详情版本、元数据和小摘要覆盖。同 source digest 不代表摘要覆盖相同；不要错误复用持久索引。事实准备完成与原文持久化共同决定保护 pin 的释放。

AnalysisWorker 在同一 Compute 线程中分片，保留最新小描述符；旧任务释放输入后才捕获替代输入。商品编译缓存按冻结目录身份/revision、分析版本、日期与材料元数据复用，账号条件逐轮重建。内存账本包含编译缓存及仍被外部持有的结果/过期视图。

PetTableModel 只接受已发布或显式注入的 PetMetadataView，并使用当前版本 facts。无 facts 时保持待确认，不因单元 fixture 而恢复 GUI 培养计算。搜索、排序、绘制消费缓存角色，账号或元数据失效要传播到筛选与当前详情。

freshnessProjection 在新建过期副本时先申请额外预算，同一源与相同失效标记复用一份投影；fresh/noop 保持 Qt 共享。申请失败显式反馈，不能返回未经失效处理的绿色建议。旧外部视图的 lease 必须延续到最后引用销毁。

关闭先停止捕获、撤销发送意图，再取消/收尾服务。正常关闭总等待有界；超时保留仍活对象和线程直到其结束，不能强杀或提前销毁，也不声称退出时所有排队记录一定保存。

## 来源、未知与写入

精灵唯一键是账号加协议实例字段 id；r/ri、fr、名称、图片和位置都不是实例 ID。兼容性通过、已登录、有本地缓存、收到同名命令分别是不同证据。

SessionSourceEvidence 区分 identityVerified 与 orderingVerified。当前生产适配器不能凭扩展本地计数或延时伪造主机顺序屏障。缺少可信来源/顺序时，应保留只读观察并明确 Unknown 或 Uncertain；不为让功能“看起来能用”绕过持久化或移动授权。

命令白名单、字段形状、严格整数、账号/实例/任务关联和会话有效性必须共同满足。队列溢出或来源中断会撤销待发送意图，不能把不同账号的迟包关联到新请求。写操作只发送一次，超时使用只读核对，不能自动重试写命令。

缺字段不等于零；保存过一份 partial 原文不等于拥有完整培养事实。限次字段属于日/周/活动周期，但周期名称本身不证明重置时点；未确认周期不能绿色显示已完成或可兑换。

## 数据与迁移

运行默认根为客户端目录的 KQPetData；没有自动 v2 子目录。KQPET_DATA_ROOT 可指定绝对路径，隔离预览应明确设置。RuntimeOptions.dataRoot 传递到 Storage/Repository/投影；多个模块不可各猜一个目录。

数据主要为 accounts\{安全账号目录名}\inventory.json、details\{id}.json、shops.json、routines.json、asset-analysis.json、snapshots，以及 derived\pets 可丢弃派生索引。外部目录在 catalog；图片按资源版本保存；日志在 logs/run-*，latest.json 只是索引。

部署、回退、卸载工具不修改个人缓存。运行扩展会更新所选根目录下兼容的 schema 3 数据，所以“工具保留数据”不能写成“运行永远不写旧数据”。日常部署与隔离预览应分目录、分数据根。

旧 schema 1 cache-v1.json 是只读导入来源，不删除源，也不覆盖已有目标事实。没有环境变量时使用 QStandardPaths::GenericDataLocation 下 KQPetInventory；有 override 时默认来源为 {data-root}\legacy-import，可通过 RuntimeOptions 显式指定。last-account 只是本地提示，不构成在线身份。

Storage 的 Queued、Saved、Superseded、QueueFull、LockUnavailable 和失败分别处理。不得把提交成功描述为保存成功。内容摘要由 I/O 对实际内容生成，Core 不序列化大 JSON 只为计算摘要。日志不默认收集原始协议、账号明文或密钥，复制诊断隐藏个人路径。

## 构建与证据闭环

~~~powershell
Set-Location 'D:\奥奇工程\KQPetInventoryExtension'
.\scripts\build.ps1
ctest --test-dir .\build-v2 -C Release --output-on-failure
~~~

特定切片可显式给 -BuildDirectory 与 -Targets。不使用已过时的 build-qt663 默认目录，也不把不同目录的加载器、DLL 和测试程序混在一起。共享头结构变动后要确保所有依赖真正重编译；不能用旧 ABI 可执行文件得出回归结论。

发布路径是 test-release → 绑定报告 → package-release。test-release 会重新构建全部目标；文档、脚本和测试也是构建身份输入，任何后续更改都会使旧报告不能证明新产物。读取 identity 应放在最后一次构建之后。

预览包使用 -Preview，仅部署隔离客户端。正式包需要同产物 -AcceptanceReport，且真实客户端、DPI、回退、原版 EXE 不变的证据完整。性能报告支持 practical review/advisory，不要求无条件跑完全矩阵；测试报告仍需记录实际已运行的注册测试，不能将跳过伪装为通过。

脚本参数和完整操作步骤见 [README](../README.md)。关键入口：

- scripts/build.ps1：默认 build-v2。
- scripts/test-release.ps1：完整构建、哈希绑定与 CTest 报告。
- scripts/package-release.ps1：配对身份、测试与正式/预览验收门禁。
- scripts/deploy.ps1：不可变版本暂存、旧配对备份、激活；运行时只暂存，关闭后重跑。
- scripts/rollback.ps1：已有 v2 release 或已核验 -Legacy 配对回退。
- scripts/uninstall.ps1：移除根入口，保留原版、个人数据、版本和备份。

验证中断、损坏 manifest、来源不明、锁冲突或报告不匹配时保留原状并反馈，不靠手工替换 DLL、删除个人文件或强杀游戏消除报错。

## 收尾状态

不要在这份静态交接文档里维护易过期的“全部通过”数字。最终以实际构建目录、报告和产物 identity 核查。

已完成（有自动检查守住）：

- 精灵页与商店页的详情只走 PreparedDetail 与只读契约；旧的同步详情分析/渲染路径已删除。
- UI 不再反向依赖 ApplicationCore/Catalog：`ui_boundary` 检查 KQPetUi 的链接与包含，`domain_boundary` 与 `/WHOLEARCHIVE` 链接检查 Domain。
- 源码已按层分目录、include 统一为相对 `src/` 的路径，见 [architecture.md](architecture.md)。

仍需在真实客户端上核实：

- 工作台入口、登录会话变化、真实请求/响应关联、旧选择清理、正常退出是否在同一配对产物上验收。
- 100/125/150/200% 和小窗口实际截图是否检查，性能抽测是否说明数据规模、缓存状态与测量限制。
- 预览包与正式包是否区分，是否完成中断恢复、v2 回退与冻结旧配对回退，原版 EXE 哈希是否未变。
