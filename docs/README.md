# 文档索引

安装、构建、打包和部署命令见仓库根目录的 [README](../README.md)，版本变化见 [发行说明](../RELEASE_NOTES.md)。

## 功能与数据

| 文档 | 内容 |
|---|---|
| [本地缓存与手动更新](local-cache-and-manual-updates.md) | 数据目录、刷新方式、缓存管理、备份与迁移 |
| [手动公共数据更新](manual-public-data-updater.md) | “检查数据更新”覆盖哪些官方资料、便携工具、图片 |
| [精灵分析的刷新与缓存](pet-analysis-lifecycle.md) | 单只分析与资产汇总在什么时候更新 |
| [战斗力组成与至高判定](pet-power-composition.md) | 官方当前/极限、持有可达、至高四个数的含义与计算边界 |
| [养成缺口与对应材料规则](cultivation-material-rules.md) | 元魂、神源兽、星轮的需求与材料对应 |

## 协议与官方资料

| 文档 | 内容 |
|---|---|
| [精灵战力计算体系](精灵战力计算体系.md) | 各时代战力公式、11 个分项、数值表、逐时代构成、回包字段语义、实证校验（原三份分册已合并于此） |
| [源兽材料背包读取](source-beast-inventory-protocol.md) | `3_11` / `2_32_0` 两个只读查询与数据结构 |
| [限时活动指定精灵兑换目录](activity-exchange-public-data.md) | 活动的发现入口与识别规则 |
| [活动兑换次数与资源观察](activity-exchange-observations.md) | 各活动次数、费用与资源的只读查询映射 |

## 设计与开发

| 文档 | 内容 |
|---|---|
| [架构与代码组织](architecture.md) | 源码分层与依赖边界、构建片段、测试布局、验证整理不改变功能的方法、路径迁移对照 |
| [设计与逆向依据](设计与逆向依据.md) | 加载链、客户端版本自适配、协议与身份、捕获来源、移动与保存、数据迁移 |
| [工程交接](AI工程交接文档.md) | 接手时的阅读顺序、线程与版本约定、来源与写入原则、收尾状态 |
| [性能基准工具](performance-benchmark-v2.md) | `run-performance.ps1` 的数据集、测量方法与结果解释 |
| 各目录说明 | [Domain](../src/domain/README.md)、[Storage](../src/storage/README.md)、[诊断存储](../src/storage/diagnostic_readme.md)、[原文记录缓存](../src/application/pet/pet_record_cache.md) |

## 证据快照

某一时刻对官方资源或客户端文件的核对结果，数值以当时版本为准。

| 文档 | 内容 |
|---|---|
| [KQPro V1.1.4 二进制兼容证据](compatibility-v1.1.4-evidence.md) | 三个桥接入口的签名、语义与字节安全性 |
| [观察周期与时间证据](observation-period-evidence.md) | 商店/日常限次周期能否自动判定有效 |
| [2026-09-13 精灵元数据更新](official-pet-metadata-20260913.md) | 精灵字典版本与摘要 |
| [指定精灵兑换目录更新](shop-catalog-source-20260913.md) | 主商店兑换目录的来源与筛选条件 |

## 历史记录

记录当时的计划、过程和结论，不再随代码更新；其中的源码路径按 [architecture.md §8](architecture.md#8-路径迁移对照2026-09-19) 对照。

| 文档 | 内容 |
|---|---|
| [v2.0 重构方案](v2.0-reconstruction-plan.md) | 重构的目标、边界、协议与验收约定 |
| [v2.0 实施记录](v2.0-implementation-log.md) | 基线、集成与真实客户端联调过程 |
| [v2.0 边界审计](v2.0-boundary-audit.md) | 2026-09-12 的依赖审计与拆分清单（已完成） |
| [修复与性能优化报告](fix-perf-report.md) / [逐项日志](fix-perf-progress.md) | 任务书 T0–T10 |
| [2026-09-18 代码评审](code-review-20260918.md) | 一轮通审的修复与误判记录 |

> 部分文档路径被程序数据引用（`src/protocol/packet_contract.cpp` 的命令白名单证据、`profiles/targets.json` 的 `evidenceReference`、`scripts/run-performance.ps1`）。改名或移动这些文档前先看 [architecture.md §4](architecture.md#4-代码约定)。
