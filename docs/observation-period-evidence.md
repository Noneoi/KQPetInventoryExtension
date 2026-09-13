# 观察周期与时间证据（2026-09-12）

结论：目前没有足够证据给商店 `siN:dl/wl/ml/pl/tl` 或日常活动观察自动签发周期有效性。数值可只读显示；有限次数参与综合建议前必须有绑定该次观察的可信周期证据。缺证据、跨周期或时钟异常均为 Unknown；不能把旧的“次数耗尽”继续当作新周期的排除条件。确证不限次的商品不依赖周期证据。

本次只读取本机已有官方解包，没有新增游戏查询或修改官方文件。以下模块版本取自现有解包目录：

| 文件与版本 | 查到的内容 | 能证明的范围 |
|---|---|---|
| Util `util~2026060598453137`，`mmo/common/DateUtil.as` 25–29、46–58、101–115、339–377 行 | 使用服务器初始时间加 `getTimer`；固定时区偏移 `-480`；部分辅助函数使用 02:00 延后日界 | 证明客户端时间/日期辅助函数的实现；不能证明每一种配额在该日界重置 |
| 主程序 `play~2026081364897007`，`mmo/play/BootLoader.as` 283–292 行 | 独立时间响应的 `t` 传入 DateUtil 初始化 | 存在原版主动获取服务器时间的流程，且它不是登录账号字段本身 |
| SocketClient `socketclient~2025071722745937`，`Commands.as` 28 行、`SocketClient.as` 645–648 行 | `getCurrentTime` 来自 `UserExtension` | 可作为以后受验证来源下的被动时钟取证线索；本扩展不为此增加查询 |
| 商店框架 `storeexchangeframework~2026081364897007`，`SEFModel.as` 101–111 行 | 目录上下架使用 DateUtil 解析日期并与服务器时钟比较 | 目录业务日期与配额周期是不同语义；不能把本机日期当作服务器周期证明 |
| 日常任务 `dailytaskservice~2026061270290269`，`DiamondTaskModel.as` 49–76 行 | 接收 `av/wav/bi/ti/wbi/wti/wdti` | 区分日、周数据组；这段响应没有给出 periodId 或 expiry 的证据 |

上述文件 SHA-256（按表顺序，SocketClient 两文件分别列出）：

```text
DateUtil.as         E252B41034BC509F68989B64755056AFC2759851BA60AA6B24E0F493C178AA4C
BootLoader.as       BF25A899BAFC6184F8594029A2D481DC8B9B2D4FEA49D5BF288E2005EA7194B3
Commands.as         5F3C4CF91609A954567E07B4122CCA363110FEB20163C214F2BB016636D4EC43
SocketClient.as     508750264FF8986540F3DDB499EFB681C500F9394876FBD690D58507E743EB31
SEFModel.as         163B8879E4E951936EC400F6C3C6D4F579B4DD3EA24AC3DDB0C74D7900C488E1
DiamondTaskModel.as 27E3B5814380136E7803E133295D643CE5C304926841FDCCE146AA2553577B1D
```

`ObservationFreshness` 的有效性输入是 Application 能力，不能由普通入站 JSON、目录文件或 UI 自行声明。它要求账号、会话 epoch、数据组、接收序号、period key/id、有限 UTC 边界、已核验服务器观察时刻及误差范围、证据引用。组级证据必须确实覆盖该组的所有相同配额键，不能把某一商品的期限扩展到其它商品。

期限由服务器观察时刻至有效期边界的差投影到捕获时的单调时间，包含服务器时钟/传输误差。后续墙钟与单调时钟的偏移或系统时区变化使旧观察失效；稳定但初始错误的本机墙钟不能代替服务器时间锚点。没有任意 TTL，没有本机午夜配额重填，也没有自动补查。Runtime 检查及分析捕获、发布时检查只更新本地有效性。
