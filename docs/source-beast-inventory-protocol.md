# 源兽材料背包读取

核对日期：2026-09-13。这里只增加查询；不会装备、升级、突破或消耗源兽。

## 查询与数据结构

手动点击“刷新材料背包”时，发起两个独立查询：

| 用途 | Extension | command | JSON 参数 | 回包字段 |
| --- | --- | --- | --- | --- |
| 元魂与精华等道具 | `MaterialExtension` | `3_11` | `{}` | `4: [{i,n,...}]` |
| 空闲源兽仓库 | `PJXExtension` | `2_32_0` | `null` | `eps: [{dpi,exp,lvl,eqn}]` |

`dpi` 是源兽定义 ID，`lvl` 是等级，`exp` 是经验，`eqn` 是这组源兽的数量。按 `dpi` 合计 `eqn`，不把数量展开成实例，也不把 `eqi` 实例 ID 当数量。不同等级、经验分组都参与汇总；同一完整分组重复、缺少必要字段、非法数字或溢出时，该次源兽观察无效，保留上次数量。

`eps: []` 是明确空仓库，可以更新为已知 0。没有 `eps`、请求拒绝、格式错误或超时都不是 0。两个查询独立更新时间和保存，全部结束后按钮退出刷新状态。

不再从 `3_11` 的可选 `24` 分组推断可消耗源兽。商店兑换次数刷新不会顺带更新这份材料观察。

## 官方静态证据

当前公共入口：[`start.xml`](https://aoqi.100bt.com/play/start.xml)。核对到的相关资源：

- [`equipment4petver2service~2026082767617311.swf`](https://aoqi.100bt.com/play/equipment4petver2/equipment4petver2service~2026082767617311.swf)
- [`interfaces~2026091011614836.swf`](https://aoqi.100bt.com/play/library/interfaces~2026091011614836.swf)
- [`sacredequipmentservice~2026082767617311.swf`](https://aoqi.100bt.com/play/sacredequipment/sacredequipmentservice~2026082767617311.swf)

对应静态类及规则：

- `E4PV2_Client.getEquipments`：发送 `2_32_0`，参数 `null`。`E4PV2_Manager.startInit` 将回包 `eps` 交给仓库数据层。
- `Equipment4PetPack.createByInfo`：依次读取 `dpi`、`exp`、`lvl`、`eqn`。
- `E4PV2_Data.handleGetEquipmentInfo` 保存仓库堆叠列表；增加/删除广播分别通知 `Equipment_Add_to_WareHouse`、`Equipment_Remove_From_WareHouse`。这是空闲仓库，不与宠物已装备槽混合。插件也不扫描或相加精灵详情的 `eps`、`shenjue`。
- `SE_MainPanel.onTabChange` 从仓库服务取得可选源兽，按当前神源兽的 `sourceId` 过滤，没有限定只能 1 级。实际升级选择保留每只源兽的 `defineId:level`。因此培养成本 `24:sourceId:1:count` 中的 `1` 不应被错误用作“只统计一级仓库源兽”的条件。
- `ExtMap` 中命令前缀 `2` 对应 `PJXExtension`。

只注册 `2_32_0` 为主动只读命令。尤其没有注册 `2_32_10`：官方该命令同时出现在历史分页及突破入口，不能为了读取仓库放行它。

## 会话与持久化

仅接收本次显式请求后、同账号及当前会话匹配的响应。未请求的弱来源包不会更新材料库存；有效只读观察也不会提升游戏写操作权限。

继续使用账号目录中的 `cultivation-materials.json`，schema 升为 2，源兽来源标识为 `2_32_0.eps-warehouse`。旧 schema 1 的元魂与精华继续恢复；旧 `24` 数量不作为“可消费源兽”显示，等待一次手动仓库查询后替换。每种材料保存独立观察时间，加载中的旧缓存不能覆盖刚收到的新观察。

## 定向覆盖

`cultivation_material_inventory_smoke` 覆盖双查询、错会话拒绝、不同等级源兽合计、忽略通用材料及宠物装备旁支、完整空列表、缺字段/重复分组保旧、部分超时、旧缓存迁移与离线恢复。客户端交互仍由用户手动验证。
