# 架构与代码组织

本文说明源码怎样分层、各层之间允许的依赖、构建与测试的组织方式，以及怎样确认一次整理没有改变功能。运行时行为、协议和数据证据见 [设计与逆向依据](设计与逆向依据.md)；接手工作的注意事项见 [工程交接](AI工程交接文档.md)。

## 1. 两个进程侧

| 侧 | 产物 | 源码 | 说明 |
|---|---|---|---|
| 原生启动侧（无 Qt） | `KQPetBootstrap.exe`（打包后为客户端根的 `KQPetLauncher.exe`）、版本加载器 `KQPetLauncher.exe`、`KQPetReleaseCheck.exe`、`KQPetCompatibilityCheck.exe` | `src/bootstrap`、`src/loader`、`src/runtime`、`src/compatibility` | 稳定入口校验 manifest 与激活记录，版本加载器检查客户端接口后启动原版并加载配对 DLL |
| 进程内扩展 | `KQPetInventory.dll` | 其余 `src/*` | 在原版 KQPro 进程内运行的 Qt 工作台 |

## 2. 源码分层

依赖只能指向表中更靠上的层（Domain 最底层）。

| 目录 | CMake 目标 | 职责 |
|---|---|---|
| `src/domain/` | `KQPetDomain` | 纯 QtCore 值与规则：战力、培养需求、时代、商店条件、推荐。没有 I/O、单例、线程和隐式业务时钟 |
| `src/contracts/` | （头文件） | 跨层共享的冻结值契约：记录、派生事实、详情、观察、操作结果、刷新时序 |
| `src/storage/` | `KQPetStorage` | 单 I/O 线程、冻结写入上下文、原子提交、诊断存储 |
| `src/diagnostics/` | `KQPetDiagnostics` | 构建信息、诊断日志、目标 Profile 守卫 |
| `src/protocol/` | `KQPetProtocol`、`KQPetInbound`、`KQPetTransport` | 封包契约与命令白名单、会话上下文、入站队列、出站传输 |
| `src/application/views/` | `KQPetReadViews` | GUI 的只读端口（`*_read_view.h`）与线程安全投影 |
| `src/application/{catalog,pet,shop,routine,analysis,common}/` | `KQPetApplicationCore` | 目录快照、精灵仓库、控制器、缓存、资产分析的输入采集 |
| `src/application/analysis/analysis_worker.*` | `KQPetAnalysisWorker` | 在 Compute 线程分片执行 Domain 分析 |
| `src/application/images/` | `KQPetImages` | 图片请求调度、解码与落盘（QtGui/Network） |
| `src/application/runtime/`、`views/*_publisher.*` | `KQPetApplication` | `ApplicationRuntime` 装配、发布器、数据更新与缓存管理服务 |
| `src/bridge/` | `KQPetBridge`（+ `KQPetMinHook`） | 内联钩子、原版分发/发送桥接、原版窗口定位 |
| `src/ui/{workbench,pet,detail,shop,routine,analysis,common}/` | `KQPetUi` | 工作台、各页窗口、模型、详情渲染 |
| `src/extension/` | `KQPetInventory`（DLL） | `dllmain`、`ExtensionContext` 装配根、嵌入资源 |

`KQPetCore` 是 `KQPetApplicationCore` 的兼容别名，不是 Domain。

### 边界检查

- `tools/build/check_domain_boundary.py`：Domain 及三个审查过的值契约只能包含 QtCore 值头和标准库，禁止 Repository/Controller/Catalog 单例、文件/网络/界面 API、Win32 适配和隐式时钟。`tests/domain/domain_core_smoke.cpp` 另以 `/WHOLEARCHIVE` 单独链接 Domain，防止静态库里藏着未解析的依赖。
- `tools/build/check_ui_boundary.py`：`KQPetUi` 只能链接 Domain、ReadViews、Images；只能包含列出的只读端口与值契约，不能出现运行时实现类型、`Catalog::instance()` 或在界面里做培养计算。

两者都注册为 CTest（`domain_boundary`、`ui_boundary`），新增跨层依赖需要同时修改检查清单并说明理由。

## 3. 线程与所有权

`ApplicationRuntime` 在 GUI 线程管理生命周期；Core 线程拥有 Repository、控制器和调度状态；Compute 执行纯派生与分析；I/O 执行磁盘、编码、摘要和图片网络任务。GUI 只通过 `InventoryProjection`、`AnalysisProjection` 与冻结 DTO 读取结果。详细约定见 [工程交接](AI工程交接文档.md) 的“线程、版本与所有权”。

## 4. 代码约定

- **include 路径**：同目录文件直接写文件名，其余一律写相对 `src/` 的完整路径（如 `"domain/pet_era.h"`、`"application/pet/pet_repository.h"`）。构建只登记 `src/` 一个项目根（测试另加 `tests/`），不同层的同名头文件不会互相误命中。
- **大类按职责分文件**：一个类的实现过大时，按职责拆成 `<类>.cpp` + `<类>_<职责>.cpp`，只在这些文件之间共用的辅助函数放在 `<类>_internal.h`（不是公开接口）：
  - `application/pet/pet_repository{,_packets,_storage}.cpp`：记录与视图 / 封包解析与会话 / 账号缓存读写与迁移
  - `application/pet/pet_refresh_controller{,_details,_move}.cpp`：核心与列表刷新 / 仓库详情批量刷新 / 背包仓库移动
  - `application/shop/shop_exchange_controller{,_activities,_materials}.cpp`：次数查询主流程 / 活动兑换 / 材料背包
- **共用组件**：商店与日常控制器的只读请求收发（排队、回执、超时、响应归属）由 `application/common/read_only_request_tracker` 统一实现；界面共用的显示文本在 `ui/common/display_text.h`；精灵表格的样式与搜索高亮在 `ui/pet/pet_table_view`。
- **证据字符串**：`packet_contract.cpp` 的命令白名单、`profiles/targets.json` 的 `evidenceReference` 引用了若干文档路径。它们属于程序数据，移动这些文档需要同时评估对运行时文本和 Profile 摘要的影响。

## 5. 构建

`CMakeLists.txt` 只做项目级设置并按层 `include()` 片段；片段与顶层同一目录作用域，相对路径和 `CMAKE_CURRENT_{SOURCE,BINARY}_DIR` 都指向项目根。

| 片段 | 内容 |
|---|---|
| `cmake/build_identity.cmake` | 版本头、构建输入清单与源码摘要、`releaseId`、identity 资源 |
| `cmake/native.cmake` | Compatibility、Startup、ReleaseCore、加载器、稳定入口、检查工具 |
| `cmake/core.cmake` | Domain、Storage、Diagnostics、Protocol、ReadViews、Application 各库 |
| `cmake/extension.cmake` | MinHook、Bridge、UI、嵌入资源、`KQPetInventory.dll` |
| `cmake/tests.cmake` | 全部注册测试，分节与 `tests/` 子目录一致 |

源码摘要覆盖 `src`、`assets`、`profiles`、`tools`、`scripts`、`tests`、`docs`、`cmake`、`third_party` 及顶层说明文件，所以**修改文档也会改变产物 identity**，测试报告需要在最后一次修改之后生成。

`tools/build/` 是构建期工具（Profile 生成、两项边界检查）；`tools/` 根目录下的更新器与生成脚本会嵌入 DLL，供“检查数据更新”使用，它们之间按同目录相互引用，不能移动到子目录。

## 6. 测试

| 目录 | 内容 |
|---|---|
| `tests/domain/` | 纯 Domain，不注册 RCC、不创建 Repository |
| `tests/application/` | 目录、仓库、控制器、缓存、分析、运行时 |
| `tests/ui/` | 表格/搜索/详情刷新，以及离屏自检的界面预览 |
| `tests/protocol/`、`tests/bridge/`、`tests/storage/`、`tests/diagnostics/` | 对应层 |
| `tests/native/` | 兼容核心、加载器、启动通道、发行清单（无 Qt） |
| `tests/performance/` | 资产分析性能（完整矩阵见 `scripts/run-performance.ps1`） |
| `tests/python/` | 公共数据更新器与缓存管理脚本 |
| `tests/support/`、`tests/fixtures/` | 公共辅助与脱敏样本 |

```powershell
.\scripts\build.ps1
ctest --test-dir .\build-v2 -C Release --output-on-failure
```

## 7. 怎样确认整理没有改变功能

2026-09-19 的目录与代码整理按下面的方法验证，以后做同类整理可以照做：

1. 整理前在独立构建目录做一次完整构建和全部测试，作为基线。
2. 整理后同样构建和测试，结果必须一致（当时为 72/72）。
3. 比对两次构建的 `build.ninja` 和 `ctest --show-only=json-v1`：每个源文件的编译宏与选项、每个目标的对象与链接库、每项测试的命令与属性必须一致（include 目录除外）。
4. 移动或拆分的函数用脚本逐个核对原文仍然存在；对不上的必须正好是有意修改的部分，并逐行审阅。
5. 比对发行产物的导出表、段大小和内嵌字符串，差异应只来自构建身份与路径派生的内部符号名。

## 8. 路径迁移对照（2026-09-19）

历史记录类文档中的源码路径是当时的位置，可按下表查到现在的文件。

| 原目录 | 现目录 | 文件（不含扩展名） |
|---|---|---|
| `src/application/` | `src/application/analysis/` | analysis_environment, analysis_worker, local_stargod_statistics_service, recommendation_adapter |
| `src/extension/` | `src/application/analysis/` | account_analysis_state, asset_analysis_controller, asset_analysis_settings, asset_analyzer, asset_snapshot_store |
| `src/application/` | `src/application/catalog/` | catalog_business_date, catalog_io_service |
| `src/extension/` | `src/application/catalog/` | pet_detail_catalog, routine_overview_catalog, shop_exchange_catalog |
| `src/application/` | `src/application/common/` | observation_freshness |
| `src/extension/` | `src/application/common/` | controller_cache_storage |
| `src/application/` | `src/application/images/` | image_service |
| `src/application/` | `src/application/pet/` | pet_derivation_cache, pet_detail_preparation_service, pet_record_cache |
| `src/extension/` | `src/application/pet/` | move_operation, pet_refresh_controller, pet_repository |
| `src/extension/` | `src/application/routine/` | routine_overview_controller |
| `src/application/` | `src/application/runtime/` | application_runtime, cache_management_service, data_update_service, persistence_summary, runtime_types |
| `src/application/` | `src/application/shop/` | shop_legacy_adapters |
| `src/extension/` | `src/application/shop/` | shop_exchange_controller |
| `src/application/` | `src/application/views/` | analysis_projection, analysis_publisher, analysis_read_view, inventory_projection, inventory_publisher, inventory_read_view |
| `src/extension/` | `src/bridge/` | inline_hook, original_bridge, original_window_locator |
| `src/application/contracts/` | `src/contracts/` | operation_types, refresh_timings |
| `src/extension/` | `src/diagnostics/` | build_info, diagnostic_logger, target_compatibility_guard, target_profile, target_profile_registry |
| `src/application/` | `src/protocol/` | inbound_queue, protocol_transport |
| `src/extension/` | `src/protocol/` | packet_contract, session_context |
| `src/extension/` | `src/runtime/` | version.h.in（模板） |
| `src/extension/` | `src/ui/analysis/` | asset_analysis_filter_proxy_model, asset_analysis_model, asset_analysis_window, recommendation_model, snapshot_history_model |
| `src/extension/` | `src/ui/common/` | pet_facts_ui, pet_image_cache |
| `src/extension/` | `src/ui/detail/` | html_document, pet_power_analysis_renderer, pet_raw_data_tree, prepared_pet_detail_renderer, stargod_ring_object |
| `src/extension/` | `src/ui/pet/` | pet_filter_proxy_model, pet_search, pet_table_model, pet_window |
| `src/extension/` | `src/ui/routine/` | routine_overview_window |
| `src/extension/` | `src/ui/shop/` | shop_window |
| `src/extension/` | `src/ui/workbench/` | pet_settings_dialog, workbench_types, workbench_window |

另外删除了 `src/extension/{asset_analysis_types,asset_analysis_version,recommendation_types}.h` 三个转发头（直接包含 `domain/` 下同名文件）；测试文件按层移入 `tests/` 子目录，构建期工具移入 `tools/build/`。
