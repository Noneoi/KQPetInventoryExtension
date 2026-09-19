# 养成缺口与对应材料规则

本说明记录开发依据，分析页面不展示逐词释义或星神逐槽计算过程。账号状态仍只来自已取得的详情与手动刷新的材料背包；本文件中的例子不是用户当前账号状态。

## 官方来源

2026-09-13 手动开发核验时，官方入口 `https://aoqi.100bt.com/play/start.xml` 返回入口版本 `20260910814679630`。资源版本由该入口对应的版本清单给出，不按本机日期猜测。

- [元魂服务](https://aoqi.100bt.com/play/petbadge/petbadgeservice~2026082767617311.swf)：内嵌 XML 的职业等级和专属激活材料，`PetBadgeMainPanel` 的目标等级索引。
- [神源兽服务](https://aoqi.100bt.com/play/sacredequipment/sacredequipmentservice~2026082767617311.swf)：`SE_SacredEquipmentConfig`、`SE_StarUpgradeConfig`、`SE_StageUpgradeConfig`、`SE_Helper`、`SE_Controller`、`SE_Model`。
- [物品服务](https://aoqi.100bt.com/play/material/materialservice~2026091011614836.swf)：`Equipment4PetItemService` 中实际消耗源兽名称和 `3_11` 数量读取。
- [基础物品表](https://aoqi.100bt.com/play/library/materialdata~2026091011614836.swf)、[增量物品表](https://aoqi.100bt.com/play/library/materialdataupdate~2026091011614836.swf)：`Item`、`ItemForBadge`、`ItemForEssence` 的实际物品名称。
- [星轮服务](https://aoqi.100bt.com/play/astrolabe/astrolabeservice~2026082767617311.swf)：节点 `lightUpCost`、专属标志、战力与位置类型。

官方新闻也明确将对应源兽、专属元魂、星轮精华、红星、万变红星作为不同养成项目，例如[灵初薄伽丘与塔梨公告](https://aoqi.100bt.com/xinwen/321365.html)。具体数量使用服务配置，不能从活动礼包数量反推培养消耗。

## 元魂

职业元魂 `type=0` 的每个 `levels[level].cost` 是**升到该目标等级**的消耗。当前为 L 级、目标 M 级时，累加 `L+1 ... M`，不把当前等级成本再次算入。

专属元魂 `type=1` 的 `activationCost` 是觉醒该槽位的实际材料。未觉醒槽位才计入；已觉醒不重复计。材料 ID 来自配置，不能使用槽位的元魂定义 ID 代替。例如元魂定义 647/648 分别是两个效果，但消耗均为物品 `4:1450:1`，真实名称“解神元魂”。两个槽位均未觉醒则合计 2 个。`35` 实际是战斗卡面类型，不是元魂。

解析器必须同时识别 `ItemForBadge`，否则专属元魂物品名会整批丢失。职业升级使用的经验元魂及其它公共培养材料保留在元数据中，用户所要求的材料摘要只展示对应专属元魂，不铺开金币等公共材料。

## 神源兽

详情槽位给出当前星级、当前阶级、`starUpgradePlanId` 与 `stageUpgradePlanId`。同一种神源兽可被不同精灵使用不同培养计划，不能只按神源兽 ID 选上限。

升星/升阶 `levels[level]` 表示**从该档位升到下一档位**的消耗。当前 L、目标 M 时，累加 `L ... M-1`，与元魂等级规则的偏移不同。

- 升星读取 `sacredStarPlans`。其成本为神觉源灵、金豆等公共材料，不凭“差几星”推断差同名源兽。
- 升阶读取 `sacredStagePlans`。每档 `equipmentCount` 可能为 0、1、2 等，必须逐档相加。计划 1 从 1 阶到 5 阶的对应源兽成本为 `1+1+1+0=3` 只；计划 13 对应阶段为 `2+2+2+0=6` 只。不能一律每阶一只。
- 对应材料串由官方 `SE_Helper` 构造为 `24:sourceId:1:count`。`sourceId` 来源于 `sacredEquipment[defineId]`；显示名称 `sourceName` 取物品服务的这个 sourceId，不能从神源兽当前名称截字猜测。
- 普通物品服务暂未列出的源兽材料，使用当前官方 `equipment4petver2/equipment4petver2service` 的 `E4PV2_EInfos` 按 ID 补名。例如该表明确包含 `E4PV2_EInfo(114,"神·薇斯佩拉源兽",...)`，因此神源兽 1114 的消耗源兽 114 已获得准确名称；不会从相似名字猜测。新增源兽自动沿用这一补充源。

材料背包 `3_11` 的 `24` 数组由 `Equipment4PetItemService.initMyItems` 读取 `i/n/uq`，分别为物品定义 ID、现有数量、已使用量。官方源兽升阶的不足判断使用 `getMaterialNum(type,id)`，最终读这一物品的 `quantity`，不按字符串中的等级参数过滤。因此对**源兽升阶的 extra=1** 可使用这组库存。不要将这一规则扩展到所有四段材料。

已装备/可装备源兽实例是另一套 `eqi/dpi/exp/lvl` 数据，不应并入这组可消耗材料数。`uq` 不是不同品质或堆栈键，不能按 `uq` 去重后累加；物品服务按 materialId 保存一条数量，重复 ID 是无效观察。

## 星轮

节点 ID `0`、`1` 都是有效节点：0 是星迹·核心，1 是星迹·连结。`lightUpMaterials` 是 `lightUpCost` 的结构化内容，逐个未点亮节点累计。相同材料 ID 合并，已点亮不再计入。显示只保留用户所需的对应精华名称和数量；金币、金豆等不出现在摘要。专属进化前置与突破状态单列为待完成事项，不能把进化精灵当作普通精华消耗。

## 元数据与更新

目录新增 `cultivationRuleVersion=1`。材料统一表示为 `{type,id,count}`，官方四段串额外保留 `extra`。元魂提供 `levels/activationCost`；源兽提供 `sourceId/sourceName` 与独立的 `sacredStarPlans/sacredStagePlans`；星轮提供 `lightUpMaterials`。

只在用户点击“全部检查更新”时检查官方版本。解析到完整有效的元魂/源兽/星轮/物品规则后，按原有组件原子覆盖目录；任何规则解析失败都保留原精灵目录。版本未变化时不重新下载组件，不访问账号接口。已在独立开发目录运行真实官方更新，并验证随后相同版本检查不会再次更新组件。
