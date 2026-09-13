# 限时活动指定精灵兑换目录

活动名称和活动模块不写在程序白名单中。用户点击“检查数据更新”时，从官方活动注册表、当前 HUD 和资源版本清单发现活动，再静态读取可识别的指定精灵兑换配置。该步骤不查询账号、不发送兑换指令，也不执行下载文件中的 ActionScript。

## 官方发现入口

- `config/config` 中 `mmo.config.CommonHudConfig.DATA` 提供当前 HUD 的 `NewActivityService#loadAndInitNormalActivity#别名`。
- 同一 SWF 的 `DefineBinaryData` 中，`newactivityconfig.xsd` 注册表提供活动别名、模块文件、入口类及每周发布日期。
- 取当前 HUD、官方最新发布日起最近六周的活动，以及这些模块常量池内引用的活动别名和 `btnNewAct_...` 链接。最多沿三层显式引用；超过范围会列入待解析记录。不会下载历史清单里的全部活动资源。
- `configinemergency/configinemergency` 的静态 XML 覆盖临时新增、临时下线的活动；`configselfblock/configselfblock` 的 `SelfBlockActivityConfig.DATA` 排除官方明确暂停的活动。
- 有版本号的模块仅在版本改变时重新下载。未列版本的模块采用官方 `VersManager.DEFAULT_VERSION`，注册表变化时再检查。没有指定精灵兑换的模块也记录结果，后续不会反复下载。

来源均为 [奥奇传说官方客户端入口](https://aoqi.100bt.com/play/start.xml) 返回的版本及官方 `https://aoqi.100bt.com/play/` 资源。开发核验采用 2026-09-11 活动目录和入口提供的当前补丁，使用独立开发目录，没有读写游戏账号缓存。

## 识别规则

支持静态数组、对象及静态常量引用，活动名、类名和表名可以变化：

1. `simpleParams` 中直接出现 `CommonEnhancePrize`，或 `Choice` 中包含该奖励与材料替代项；精灵范围必须是明确正整数 ID。
2. `@sb` 选择器通过官方进化配置、多形态配置和皮肤配置展开，遵循 `getPetEvoLinkAllRaceIds(seeds, false)`；不能只删除后缀，也不能按名字找相似精灵。
3. `type="Strengthen"`、`params`、明确 `filter` 数组的活动表，在同模块存在官方 `CommonEnhancePrize` 组装与筛选代码时自动识别。
4. 养成代码保留原始参数；目前支持 `33$正整数`、`39$1`，未支持的参数不擅自丢弃。
5. 必须存在可识别的兑换费用字段。签到、通行证等级奖励不进入商店；累计代币门槛领取奖励也不当成兑换扣费。

隐藏条件兑换若目标精灵和养成代码明确，会保留该项及关联条目条件，显示适用状态待确认。未解析的复杂表达式会记录活动名、模块、表、条目编号及原因。

## 缓存与商店合并

独立缓存为 `catalog/activity-exchange-data.json`，采用 `schema=1`。`shops` 是当前发现的兑换目录，`modules` 保存每个模块最新版本与识别结果，`pending` 保存待适配项目。不会逐次堆积历史版本。

活动分组的 `sourceKey` 为 `模块路径#配置类.表名/分支`；`shopId` 只在该来源内有效。条目的 `itemServerId` 保留本地编号，允许官方从 0 开始编号。活动与主兑换框架即使编号相同也不会混用缓存或次数。

商品保留 `enhanceType`、`raceIds`、`cost/costKnown/costRaw`、`shelfTime/removalTime/availableKnown`、`quotaRaw` 和条件原文。官方没有提供日期时不填本机日期；活动货币 ID 或价格档位选择未确认时不编造材料 ID、零费用或当前价格。

本次发现的 8 个活动目录包含 72 项指定精灵兑换：72 项都有静态限次额度，69 项费用固定，另 3 项按活动总购买次数选择价格档位。商店中手动点击“刷新兑换次数”，会查询其中 7 个活动目录的 68 项次数、活动币余额和价格条件，并通过公共材料查询 `MaterialExtension / 3_11 / {}` 更新标准货币、道具余额。圣冕秘阁的 4 项可显示静态费用与总限 1 次；当前次数读取仍需要宿主生成的 `ci` 参数，尚未启用，不省略或伪造参数。具体映射见 [活动兑换次数与资源观察](activity-exchange-observations.md)。

活动观察保存在当前账号的 `activity-exchanges.json`，按活动来源及查询范围隔离。V 币、回归币等活动专属币保留所属活动，不因名称或编号相同而与其他活动或公共材料合并。当前数值显示为“只读观察”，保留的旧数值显示为“上次”；读取成功不自动证明限次周期仍有效。失败保留该来源上一份有效数据与时间。启动、切页及公共数据更新都不自动查询账号，也没有后台定时刷新。

上述数量仅描述本次官方公开资源中已发现并支持的目录，不代表全部历史活动或未来所有新结构。新增同结构活动沿相同发现和解析规则更新；未支持的结构保留覆盖记录及待适配原因。

## 可复现的官方实例

| 官方资源 | 静态来源与适配形态 |
| --- | --- |
| `newactivityext/newact20260828/s2exchangeshop/s2exchangeshop` | `S2ESExchangeConfig.EXCHANGES`，指定精灵代码、种族数组、`4:3266:数量`费用、上下架日期与原始限次 |
| `newactivityext/newact20260612/holycrownsecretpavilion/holycrownsecretpavilion` | `HCSPExchangeConfig.CONFIG`，`7264@sb`、`7545@sb` 精确进化范围 |
| `newactivityext/newact20251231/chargevipexchangegift251231/chargevipexchangegift251231` | `CVEG251231Config.ArrayData`，V 币指定精灵养成 |
| `newactivityext/newact20260731/lingchunaifeimalltab5/lingchunaifeimalltab5` | `PRIZE_CONFIG` 的 `Strengthen/params/filter` 模板及关联源兽条件分支 |
| `newactivityext/newact20260828/perfectluoshiqigainfree/perfectluoshiqigainfree` | 活动内部付费养成表，包含 `39$1` 与 `33$1` |

`tests/public_activity_exchange_updater_test.py` 验证新增未来活动自动发现、增量及负结果缓存、失败保旧、编号隔离所需信息、条件条目、进度奖励排除、紧急新增及临时下线、静态解析边界和参数保留。
