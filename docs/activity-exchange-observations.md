# 活动兑换次数与资源观察

核对日期：2026-09-13。范围为当前目录中的 8 个活动来源、72 个指定精灵兑换项目。依据现有官方模块静态解包，并重新读取当前 `library/common~2026091011614836.swf` 核对通用客户端与限次解析规则。没有向真实客户端发请求，没有读取账号响应，也没有修改用户缓存。下面区分已确认的查询映射、可直接读取的资源和仍需要适配的条件。

活动次数确实有查询入口，不能从主商店 `1008_20260313_es_0` 的 `siN` 套用。费用也有两种来源：标准道具/钻石使用公共材料背包，V 币、回归币使用活动自己的返回字段。查到的次数只表示本次账号观察；次数的适用周期仍由该活动静态配置与本次有效状态共同确定。

## 查询命令及隔离

| 活动来源 | 已确认的初始化查询 | 参数 | 官方调用证据 |
| --- | --- | --- | --- |
| S2 赛季商店 | `TimelinessActExtension / 1008_20260313_es_2` | `{"i":10}` | `S2ESClient.info` → `S2ESController.getInfo`，参数来自 `S2ESExchangeConfig.SHOP_ID` |
| 回归每日任务的兑换商店 | 同上 | `{"i":9}` | `R2026T4DTClient.getInfoExchange` → `Manager.tryGetInfoExchange`，参数来自 `ExchangeShopId` |
| 回归每日任务的等级配置 | `null / 1039_3_0` | `null` | `R2026T4DTClient.getInfo` → `Model.parseDataTask`；`lv` 决定兑换配置的 `lvFlag` 是否适用 |
| 充 VIP 兑豪礼 | `TimelinessActExtension / 1008_20251231_cvgv2_0` | `null` | `CVEG251231Client.getInfo` → `Model.parseData` |
| 灵初解神特惠专场 | `SimpleActExtension / 1019_0` | `{"ai":5747}` | `LCNFMT5_Client.getInfo` 调用 `ClientSA.CMD_GET_INFO`；构造函数传入 `AI=5747` |
| 完美洛世琦全民送中的付费培养 | 同上 | `{"ai":5735}` | `PLSQGF_Client.getInfo`；构造函数传入 `PLSQGF_Config.SimpleActId=5735` |
| 回归特惠商城 | `null / 1039_4_0` | `null` | `R2026T6RSClient.getInfoShop` → `R2026T6RSDataShop.parseData`；不使用另两个 VIP/兑钻页的查询 |
| 抽卡活动内的兑换 | `TimelinessActExtension / 1008_20250627_hdc_0` | `null` | `Ospl25_Client.getInfo` 调用 `requestForGetInfo`，该通用方法把默认 `reqParams=null` 传给 `request` |
| 圣冕秘阁 | `TimelinessActExtension / 1008_20260508_cl_0` | 必须带官方生成的 `ci` | `HCSPClient.getInfo` 显式传入 `{"ci":ActUtil.getNewChangeSetId()}`；参数生成仍委托宿主 `interactHelper`，不能固定填 0、随意递增或省略 |

以上依据来自方法内部的实际调用，不是根据命令以 `_0` 结尾作推断。每个模块的购买、领取、兑换方法使用另外的命令，均不纳入本表。`HCSPController.getInfo` 的返回处理仅初始化显示模型；但它所要求的 `ci` 还没有可独立生成的契约，因此可以先显示其已经确认的静态费用和总限次，等待动态参数适配。

通用 `ClientSA` 明确把 `CMD_GET_INFO` 定义为 `1019_0`；`request` 注入 `ai`，`shouldAcceptResponse` 检查响应的 `ai` 与请求一致。插件也必须匹配该字段。`1008_20260313_es_2` 的两个 `i` 使用同一命令名，当前读取代码没有提供可靠的回显 `i` 关联证据：必须串行查询；超时后停止本轮剩余同命令组，避免迟到响应串组。

`null` 扩展是当前官方实际路由：`ExtMap` 没有 `1039` 名称项，`SocketClient.sendXtMessage` 会在扩展未注册时，从命令前缀取数值扩展 ID。不能猜造一个 `ReturnExtension` 名称。

## 已用次数、资源和价格的准确路径

| 来源 | 已用次数 | 消耗与已有资源 | 需要注意 |
| --- | --- | --- | --- |
| S2 | 顶层 `bi<serverId>.<limitKey>` | 商品的静态 `cost`；当前指定精灵项目为道具 `4:3266`，已有量来自材料背包 | 不是 `si10.biN`。`serverId` 来自商品配置；`limitKey` 来自通用限次 bundle |
| 回归每日兑换 | 顶层 `bi<index>.tl` | 配置 `daibi="id:num"`；返回 `cn="id:num#..."` 为余额。`1=回归养成币`、`2=回归精灵币` 来自 `ObjDaibiNames` | 币属于本活动来源，不是公共物品编号。另一次 `1039_3_0` 返回的 `lv` 用于 `NewBack30Day$<lv>` 配置筛选 |
| 充 VIP 兑豪礼 | 数组 `li[dataIndex]` | `c` 为当前 V 币，`cl` 为累计 V 币；商品 `daibi` 为消耗 | 使用 `dataIndex`，不可用数组中所显示的行号。`month` 为活动返回的月份信息。带 `ypFlag` 的免费条件依赖年费 VIP 状态；当前两条指定精灵项目 `ypFlag=0` |
| 灵初解神特惠 | `b<bi>.b<bi>`；有 `baseOnId` 时读取对应基础条目的两层键 | `prices[1]` 是实际钻石价格，标准货币 `8:2`；`prices[0]` 只是原价展示 | `t` 只是累计购买次数，不是价格选档依据。特殊 `bi=5` 与 `baseOnId=4` 共用基础行限次 6，不因特殊行 `limit=0` 当成无限独立额度 |
| 完美洛世琦付费培养 | `b<index>.b<index>` | `price[min(bt,2)]` 为当前钻石价，标准货币 `8:2`；`bt` 为本活动总购买次数 | 当前 `TotalPriceNum=3`；`bt=0` 用第 1 档，`bt=1` 用第 2 档，`bt>=2` 用第 3 档。不能按单商品次数选档 |
| 回归特惠商城 | 数组 `bt[index]` | 商品 `price` 为固定钻石价格 `8:2` | 返回 `ps[index]` 是宠物状态，不能用来推导已购买次数 |
| 抽卡内兑换 | 在 `p` 数组中找 `i == config.index`，读取该元素的 `l` | `cost` 为数量；物品服务读取 `COIN_ID=3200`，即公共道具 `4:3200` | `p` 是按 ID 查找的对象数组，不是按 `index` 下标读取；`l` 是已用量而非剩余量 |
| 圣冕秘阁 | 在 `pl` 中找 `id == PetCfg.id`，解码该对象 `ep="exchangeId:used#..."` | 指定精灵养成条目的 `cost` 消耗公共物品 `4:3254`，`HCSPConfig.UNIVERSAL_ITEM_ID` 明确给出 | 总限次来自 `PetCfg.exchangeIds="exchangeId:limit#..."`。当前养成项目 `0/1` 对应 PetCfg `0`、`3/4` 对应 PetCfg `1`，四项总限均为 1 |

圣冕秘阁中 `coinItemId=3258/3276` 是兑换精灵本体及任务取得的专属币，不能误用为上述养成费用；养成确认框明确读取 `model.universalCoins`，后者查询道具 `3254`。当前红星项目消耗 140，源兽升阶项目消耗 160。

灵初解神源兽升阶还有明确的条件分支：主界面读取所选精灵当前源兽的升阶材料；若其中存在 `MaterialTypes.EQUIPMENT4PET_ITEM`，使用常规 `bi=4`、实际价格 103；否则转到 `specBi=5`、实际价格 29。这个选择来自具体精灵详情和当次官方升阶计划，仅查活动账户状态不足以决定。未接入条件判断时，保留两条静态分支与各自价格、共同限次，不能任选低价。

## 缺失不是一律为零

- S2 的 `S2ESReward.initData` 明确在缺 `biN` 或该限次键时 `setData(0)`。可声明 `missingValue:0`，但只作用于已成功取得的正确活动响应。
- 灵初解神的 `parseBought` 明确在缺父对象或同名子键时返回 0，可声明相同缺省。
- 抽卡 `Ospl25_RewardData.initData` 明确在完整 `p` 列表中找不到该项目时把剩余量设为 `maxNum`，对应已用 0；`p` 本身未返回不能据此当成 0。
- 回归 R6 将未给出的 `bt` 转为空数组，并对缺下标作 `int(...)`；若采用缺省 0，应将此作为该已确认模板的规则，不能推广到其他活动。
- 回归 R4 直接访问 `biN.tl`，未给出的 `biN` 不存在通用零值证据。`cn` 明确补齐已知的两个币种为 0，但前提是 `cn` 本身是有效完整编码串。
- 圣冕秘阁只有匹配 `ep` 项时显式赋值。当前没有将“缺整个 `pl` / 缺 PetCfg 项”认定为零的证据。未适配其初始化参数前保持未知。

任何请求失败、超时、账号变化、请求分组不匹配、字段类型无效，都保留对应来源的上一份有效观察及时间，不改变其他来源。零值规则不得应用于没有收到响应的情形。

## 声明式映射契约

表中的具体命令和字段由官方类、构造参数、配置表及模型读取链提取。以下是可实现的值契约，不执行字符串表达式。

```json
{
  "observation": {
    "schema": 1,
    "requests": [
      {"key":"state","extension":"TimelinessActExtension","command":"1008_20260313_es_2","params":{"i":10}}
    ]
  },
  "quotaObservation": {
    "requestKey":"state","path":["bi4","pl"],"valueKind":"used","missingValue":0
  }
}
```

生成目录时将 `bi4`、`pl` 等替换为从该商品真实配置算出的**完整字段名**，运行时不做任意模板插值。路径中的整数表示数组下标，例如 `['li',5]`。对象数组查找需要专门步骤，例如 `['p',{'find':'i','equals':37},'l']`；找不到、找到多条和无效数组应分别处理，不能将第一条随意当结果。

活动币使用同一个查询的独立值；例如 V 币和回归币可以描述为：

```json
[
  {"name":"V币","count":30,"requestKey":"state","path":["c"]},
  {"name":"回归养成币","count":300,"requestKey":"state","path":["cn"],"encoding":"id-counts","itemId":1}
]
```

示例的数量只是映射形态，实际 `count` 必须逐条取官方 `daibi`。`id-counts` 仅解析十进制非负整数的 `id:num#...`，重复 ID、负数及不完整串拒绝整组。该币键需包含 `sourceKey`，不能并入公共 `materialCounts`。

当前价格可声明精确条件，例如洛世琦红星的三档是：

```json
[
  {"cost":"8:2:47","when":{"requestKey":"state","path":["bt"],"op":"eq","value":0}},
  {"cost":"8:2:41","when":{"requestKey":"state","path":["bt"],"op":"eq","value":1}},
  {"cost":"8:2:38","when":{"requestKey":"state","path":["bt"],"op":"gte","value":2}}
]
```

条件字段未知时保留所有价格档及说明，不声明当前价。标准材料余额仍通过明确的材料刷新读取；公共数据更新只提供定义，不以读取官方 SWF 替代账号查询。

## 新活动同模板自动映射

可复用的识别依据如下，不按活动别名维护手写表：

1. 沿界面初始化 → Manager/Controller → Client 的实际调用定位命令、参数常量。继承 `ClientSA` 时解析构造函数活动 ID 和当前公共基类的 `1019_0` 注入规则。
2. 沿 Model 传给 Reward 的数据范围，组合两层及多层路径。例如先取 `b<index>` 再在 Reward 取 `b<index>`，结果必须保留两层。
3. 只有 `Factory.createLimitedByBundle` 或明确同构逻辑才把 `0/1/2/3/4` 对应日/周/月/期/总及 `dl/wl/ml/pl/tl`；其他单独的 `limit` 数字只表明上限，不能自行标成“每日”。
4. 跟随商品的 `serverId`、`index`、`dataIndex`、`baseOnId` 各自语义，不能统一假设等于 UI 行号。共享限次保留同一个计数来源。
5. 费用从真实购买确认/提交前的选择逻辑核对；`prices` 数组可能是原价与现价，也可能是真分档，不凭字段名判断。
6. 超出支持范围的动态参数、嵌套关联、年费条件或精灵条件，保存可确认的静态额度和费用，同时给出未支持原因。新增结构不应导致其他已确认来源全部不可读。

## 证据定位

开发副本位于 `build-v2-activity-exchange-research/live-run/scratch/activity-export-*/scripts/`，各活动子包与 [活动目录说明](activity-exchange-public-data.md) 中资源路径一致。核心证据文件：

- S2：`S2ESClient.as`、`S2ESController.as`、`S2ESReward.as::initData`。
- 回归 R4：`R2026T4DTClient.as`、`R2026T4DTModel.as::parseDataExchange`、`R2026T4DTReward.as`、`R2026T4DTExchangeConfig.as::getExchangeData`、`R2026T4DTConfig.as::ObjDaibiNames`。
- 充 VIP：`CVEG251231Client.as`、`CVEG251231Model.as`、`CVEG251231Reward.as`、`CVEG251231MainPanel.as`。
- 灵初解神：`LCNFMT5_Client.as`、`LCNFMT5_Model.as::parseBought`、`LCNFMT5_Manager.as` 的两处 `prices[1]`、`LCNFMT5_MainPanel.as` 的升阶材料分支。
- 洛世琦：`PLSQGF_Client.as`、`PLSQGF_Model.as`、`PLSQGF_Reward.as`、`PLSQGF_BuyPanel.as`、`PLSQGF_Config.as::getPriceIndex`。
- 回归 R6：`R2026T6RSClient.as`、`R2026T6RSDataShop.as::parseData`、`R2026T6RSReward.as`。
- 抽卡：`Ospl25_Client.as`、`Ospl25_Model.as::getFilterRespData`、`Ospl25_RewardData.as::initData`、`Ospl25_Config.as::COIN_ID`。
- 圣冕：`HCSPClient.as`、`HCSPModel.as::initFromInfo`、`HCSPPetInfoVo.as::parseExchangeInfo`、`HCSPExchangeReward.as::initData`、`HCSPPetConfig.as`、`HCSPConfig.as`。

此次重新取得的通用类位于 `observations-readonly/common/scripts/`：`ClientSA.as` 确認只读入口与 `ai` 匹配，`ClientXT.as` 确认初始化参数默认值，`ActRewardFragmentFactory.as` 确认限次类型与键名。官方来源为 [公共入口](https://aoqi.100bt.com/play/start.xml)、[当前 Common 模块](https://aoqi.100bt.com/play/library/common~2026091011614836.swf)及活动目录记录的版本化官方 SWF。客户端实际回包仍由用户手动触发后验证。
