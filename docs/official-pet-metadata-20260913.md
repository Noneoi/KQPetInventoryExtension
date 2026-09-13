# 2026-09-13 精灵元数据更新

当前官方入口 `https://aoqi.100bt.com/play/start.xml` 返回版本 `20260910814679630`。
按版本清单及入口热更新选择资源，本次更新精灵基础字典与增量字典，共 9638 条（旧版 9542 条）。其他培养材料、元魂、星神等表没有借此变更来源版本。

| 来源 | 版本 | SHA-256 |
| --- | --- | --- |
| [基础字典](https://aoqi.100bt.com/play/pet/petdictionarydata~2026073181080477.swf) | 2026073181080477 | fc86e5396a3269ffccd93721ba770fb4d3a1e4c51803d7388da3114d3e56f666 |
| [增量字典](https://aoqi.100bt.com/play/pet/petdictionarydataupdate~2026091011614836.swf) | 2026091011614836 | 086f228382f65bc11d29c80339d97d13a2bf6bea184fe15493e7f5aecc20b621 |

`PetDictionaryDataItem.create` 的参数 9、10、43、64 分别为属性序列、职业序列、原种族组、星神槽位最高等级。精灵 `7529` 为 `[灵初]轮转·命运之轮`，属性 `26`（神灵）、职业 `32`（神攻）、星神槽位最高等级 `8`。不能把回包的 `rt=26` 当作官方职业序列，也不能根据名字尾段借用另一时代的属性。

另核对了[当前 Interfaces](https://aoqi.100bt.com/play/library/interfaces~2026091011614836.swf)中的 `PetBuild`、`PetProgressionGifted`、`PetGift`：`ip` 是包含星能加成的最终 12 项天赋值，原顺序没有改变；`gps=2/3` 分别表示单/双星能。明确的空 `gps` 按官方语义表示无星能；缺失字段或缺失段仍显示待确认。仅依据已收到的实际天赋字段，不用推荐培养配置补造当前数值。

`tools/refresh_pet_metadata.py` 可从最新官方入口重复刷新：传入 `--catalog`、`--output`、`--work-dir`、`--java` 和 `--ffdec`。可用 `--base-script` 指定已解包的基础 `PetDictionaryDataContents.as`，路径版本必须匹配当次官方清单。源码不再依赖固定的个人解包目录。生成结果记录版本、官方 URL 与哈希；旧版官方缓存不会覆盖较新的内置或已加载元数据，原缓存文件保留。未知自定义覆盖文件仍可加载。
