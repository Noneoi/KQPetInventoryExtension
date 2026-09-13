# 指定精灵兑换目录更新

2026-09-13 02:52:47 UTC 从官方页游资源获取。使用页游数据，未混入同名手游数据。

- [官方版本入口](https://aoqi.100bt.com/play/start.xml)：客户端版本 `20260910814679630`。
- [官方资源版本表](https://aoqi.100bt.com/play/versiondata~20260910814679630.swf)。
- [官方商店配置资源](https://aoqi.100bt.com/play/newactivityext/newact20260313/storeexchangeframework/storeexchangeframework~2026091011614836.swf)：SHA256 `d6a0c45d0ef5f1e243d127dee6c8d4c95423f7e9f4879d92fc9b3890b035ac89`。

筛选条件为奖励参数类型精确等于 `CommonEnhancePrize`，并提供明确、完整的正整数种族 ID 列表。商品描述只用于展示。普通材料、服饰、礼盒及没有指定种族的奖励不收录；仅含普通物品的商店 8 不新增。

目录从 21 项增加到 26 项，保留其中 1 项已过期历史配置；2026-09-13 可显示 25 项，旧目录在同一日期只有 14 项。新增 5 项：排位赛满金星套餐、排位赛红星、月福利红星、月福利满金星套餐、联盟元魂觉醒。其余商品的指定种族、成本、限次和在售日期同步使用此次官方配置。

| 商店 | 配置数 | 当日在售数 |
|---|---:|---:|
| 永恒战场 | 4 | 4 |
| 奥奇之星 | 2 | 1 |
| 排位赛 | 7 | 7 |
| 竞技场 | 3 | 3 |
| 月福利中心 | 8 | 8 |
| 联盟兑换 | 2 | 2 |

官方月福利商品 `removalTime` 原文为 `2026929`。官方 `mmo.common.DateUtil.parseDate` 使用固定位置截取再调用 AS `Date`，会归一化成 `2033-08-09`；独立的 `TOTAL_CONFIG` 商店窗口于 `2026-09-29` 关闭。生成器及运行时导入均遵循这两个条件，将可见截止日限定为 `2026-09-29`，原字符串保留在 `officialRemovalTime` 中。没有推测补零，也没有将目录日期当作服务端限次周期。

复现：JPEXS 26.2.1 导出官方 SWF 的 `SEFConfig`，生成器支持自动获取当前版本及热更新覆盖：

```powershell
python -B tools/generate_shop_exchange_data.py --refresh-official --java <java.exe> --ffdec-jar <ffdec.jar> --cache-directory <output-directory>
```

获取目录保存下载 URL、UTC 时间、资源 SHA256 和配置 SHA256。离线重放同一配置及 `source.json` 可逐字节生成同一 JSON：

```powershell
python -B tools/generate_shop_exchange_data.py --config <SEFConfig.as> --provenance <source.json> --output <shop-exchange-data.json>
```

本次生成和离线重放 SHA256 均为 `2aa1c6b6cf544eaf52f89f30fe8ddf9cd61d847c80c040b9ffc588070d06f604`。四项 Python 定向回归通过；C++ 商店/目录回归覆盖新商品的增强类型识别、新商店编号、官方日期归一化及商店关闭边界。
