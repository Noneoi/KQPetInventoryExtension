战斗回放：独立的数据核对样板

本目录的工具从原始日志和官方包建立依据，没有导入旧回放方案。

本地压缩资料包（KQBRPK1）：
在项目根目录执行：
python -B tools/battle_replay/catalog_pack.py --input assets/battle-replay-catalog.json --output build-v2/data-packs/battle-replay-catalog.brpack
必须显式指定输出路径；工具不联网、不下载、不自动安装到工作台，也不允许输出覆盖输入或输入的硬链接/符号链接别名。
输入必须是完整 format 3 JSON；检查必需表、核心字段、来源记录和汇总计数。来源检查是结构校验，不证明任意本地文件的真实性，不补写名称或机制。
固定文件头共56字节：8字节 ASCII KQBRPK1 加换行；8字节大端无符号原始JSON长度；8字节大端无符号压缩体长度；32字节原始JSON的二进制SHA-256。
其后恰好一个RFC 1950 zlib流，生成时level=9；禁止尾随字节、多流、gzip或裸deflate。包总大小最多32 MiB，展开JSON最多128 MiB，两项长度均须大于0。
JSON原始字节完整保留，含空白、顺序和可选UTF-8 BOM；不会重新序列化。展开使用明确长度上限并核对eof、尾随内容、长度和SHA-256。
生成后回读比对，在输出同目录写临时文件、flush/fsync，验证成功才原子替换；失败保留旧包。标准输出给出json_bytes、pack_bytes和sha256等JSON统计。
Python的unpack_bytes仅校验容器、解压与字节哈希；消费任意外部包后仍须validate_catalog_bytes或客户端对应的完整资料校验。
针对性测试：python -B tests/python/test_battle_replay_catalog_pack.py
正式客户端通过 CatalogIoService 在既有 Storage I/O 线程读取 KQPetData/catalog/battle-replay-catalog.brpack，无包才读同目录 battle-replay-catalog.json。坏包不改选旧JSON；已有快照保留原快照，无快照时回退内置并提示原因。当前战报固定资料版本，后续打开战报才采用更新后的资料。
C++ Decoder 每步消费最多64 KiB压缩输入、输出最多64 KiB，取消时可在步骤之间销毁；解压后仍执行完整JSON结构/来源记录校验。使用项目固定Qt6.6.3提供的Qt6::ZlibPrivate接口和QtCore导出函数，无新增运行时DLL。
构建目标KQBattleReplayCatalogPack生成构建目录/data-packs/battle-replay-catalog.brpack；KQPetInventory依赖此目标。设置更新已接通（见下文），构建本身不复制到已安装工作台。内置后备资源保留；压缩仅减少文件占用，运行时仍整份解析。
C++跨语言验收：KQBattleReplayCatalogPackSmoke <生成的.brpack路径> <源JSON路径>；CTest名battle_replay_catalog_pack_smoke。实际包、旧JSON兼容、失败保旧、完整内置回退、表数量/原文SHA与界面心跳已验收；2026-10-10结果见build-v2/validation/replay-catalog-pack-20261010/tests.xml和validation.json。

工作台公共更新接入（2026-10-11）：
设置新增“战斗回放资料”，固定组件名battleReplay，单项和全部更新均可用。DataUpdateService仅在选择该组件或全部时准备回放专用资源；public_data_updater.py惰性导入public_battle_replay_updater.py，其他类别无需这些回放文件也能使用。
完整源码、构建所需的744项原始依据与原始SHA均在assets/battle-replay-update-sources.zip（13,873,725字节）和资源表中。它不是完整研究档案，不含战报、抓包或用户账号数据。update_source_bundle.py还原独立目录，仅迁移JSON副本中的路径并按依赖顺序重算引用哈希，官方原始数据无路径变化时保留原字节。构建输入已通过禁止读取还原目录外文件的完整验证。
基线使用现有内置JSON在Storage I/O线程分段压缩为scripts/baseline/battle-replay-catalog.brpack，支持取消和QSaveFile原子保存；不落94 MB展开基线、不嵌入第二份同内容包。压缩流式不代表Qt资源底层和最终JSON解析都已改成按需加载。
Flash版本解析统一遵循start.v→versiondata→start.u.f覆盖，缺省SWF版本2000；工作台和回放共用本次快照，不再使用历史lv缓存捷径。H5核对官方main加载代码SHA，再按version表→start.files覆盖→mix别名优先，缺省20211126；工作台技能和回放共用同一快照及下载。入口在激活前再次抓取，更新中切版则保留旧包。
本轮联网核对71项Flash包和8项H5资源（含H5加载器）；用相同包SHA证明可复用消费者文本。已支持的battleconfig XML、四张SkillConfigSimple字面MAP、精灵基础/增量字典可更新；MAP允许字面数据变化，外壳代码必须不变。字典只允许datas和timestamp变化，固定字段数与编号核对；按官方严格较新timestamp合并，合法空增量不报错。其它消费者包或代码发生未知变化时整体回放组件失败，不修改批准哈希或补猜新语义。
每次需要重建时，update_generated_evidence.prepare_reference_and_race从本次技能表与字典重建消费者绑定、缺项核对和种族展开，不读取旧source-snapshots作为新输入；气运/溯灵/旧传说Flash规则在对应包字节一致时复用。其历史H5比较未全面重抓，旧时间与来源保留，不算本次最新核验；星神、元魂、星轮的Flash/H5完整字段对照会重做。
生成完整format 3后检查全部必需集合、字段、来源、汇总及核心表相对内置/当前有效包的明显缩减，再压缩、回读核对、最后同目录原子替换catalog/battle-replay-catalog.brpack。新包不裁成工作台精简技能表，未知/未解析字段与已记录边界仍保留。官方代码或结构不兼容、网络失败、格式失败及最终替换失败都保留旧包。包已生效但界面重读失败时继续保留原有效快照。
来源指纹与解析器/输入包指纹一致时跳过重建和重写，仍核对官方资源与入口稳定性。当前播放战报始终固定旧快照，新打开的战报采用重读后的资料。临时下载、展开依据、关联表、候选JSON在正常成功或异常退出路径统一清理；强制杀进程、断电可能留下未发布的update-*目录，不能当成已安装资料。
目录已与工作台共用，公共下载与Python/Java/JPEXS工具复用；目前仍保留独立完整回放字典，尚未做与精简工作台定义去重，也没有跨全部数据类别的事务回滚。研究素材的大目录无需安装。
资源并集升级：public_resource_plan.py在解析前合并工作台与回放声明、既有目录的已记录官方引用；固定81个SWF、2个FUI、17个H5文件、1个额外版本依赖，动态活动/面板/图标继续追加。public_resource_session.py提供统一有界版本解析、线程安全同次下载与SHA校验、H5 files/mix规则及切版阻断。源码必须导出public_resource_plan.py、public_resource_session.py、public_cultivation_supplement.py以及battle_replay/fetch_reference.py；最后一个有界SWF模块即使只更新工作台也需要。资源审计保存catalog/public-resource-manifest.json，不把历史记录当成本次核验。新增public_resource_plan/public_resource_session/public_skill_updater/public_cultivation_supplement四组独立测试；完整部署必须重建Launcher与DLL并通过绑定产物的test-release.ps1报告。
开发复现：将resources.qrc的data-updater资源按别名导出到独立KQPetData/data-tools/scripts，准备上述baseline包，然后运行该scripts/public_data_updater.py --data-root <独立KQPetData> --components battleReplay。不要把开发验证指向正在使用的数据目录。bundle生成API为create_bundle(read_manifest, argument_manifest, output, extra_files)，精确730项读取追踪和14项额外清单见build-v2/validation/replay-update-investigation；这只用于发布时生成材料，用户更新不需要这些开发目录。
针对性Python测试：test_public_battle_replay_updater.py、test_battle_replay_update_evidence.py、test_battle_replay_update_source_bundle.py、test_public_data_updater.py。C++验证包含设置按钮、离线公共更新门禁、单项失败隔离、更新后的重读、当前战报版本固定、关闭取消和完整真实包读取。
本轮最终结果、真实联网日志、重复检查不改原包验证、磁盘采样及产物哈希见build-v2/validation/replay-update-integration-20261011。KQBattleReplayCatalogPackSmoke可加第三个参数<本次更新.brpack>，额外通过真正的CatalogIoService验证新版完整包；测试不把它的展开JSON写入数据目录。
最终验收：16组CTest全通过；真实联网更新77.927秒，再次检查26.550秒且包字节/修改时间不变；两次均无剩余临时更新目录。新包4,982,042字节，展开原文100,964,128字节，9,722精灵/21,155技能/34,486 Buff。真实C++后台读取1.779秒，界面心跳最大间隔3毫秒，最长I/O阶段1.271秒仍是整份JSON解析。以上为本机实测，不保证其它环境耗时。
空间：DLL 28,666,880字节，比上一阶段增加14,199,808字节；新增21项更新资源导出14,284,128字节；Qt从内置原文生成的基线4,901,696字节。首次启用且这些回放资料此前均未落盘时，DLL增量加上述资源/基线及新版外部包合计38,367,674字节，复用的公共Python/Java/JPEXS不另算。本轮隔离数据根目录每秒采样最高640,302,570字节，完成后24,518,144字节（含已有公共更新脚本）；不是瞬时峰值、内存或首次工具下载测量。Qt和Python压缩基线分别为4,901,696与4,902,211字节，但展开原文及SHA完全相同；真实联网验证使用后者。具体口径均记录在validation.json。
assets/battle-replay-update-sources.zip是源码必需输入，.gitignore已为这一文件单独放行；其它临时ZIP仍忽略。交接或新检出时必须包含它，不能只复制Python文件。此阶段已完成编译及隔离验收，尚未安装到实际运行的工作台。

当前可以使用：
1. 工作台“战斗回放”页面，Ctrl+5；独立预览程序 KQBattleReplayPreview.exe。
2. 打开 txt/json 或整个样本目录；默认连续播放完整行动，自动衔接下一次出手，跳过纯准备快照；暂停、前后跳转和回合跳转仍可用。动作细节默认折叠，逐条核对可选。
3. 敌我双九宫格；保留同格多单位、召唤候选、分身、追击单位及独立队伍效果。
4. 原始文件完整保留，SHA-256、全部字段、未知字段、当前行动原文随时可查。
5. 已核实字段的中文说明、官方技能与 Buff 名称和说明。
6. 生命红条、气势橙条，条内显示当前值/参考值；护盾直接显示蓝色余额。双方紫色通灵条按日志mp分格，支持超过满值及未知值。
7. 形态、属性、当前技能、召唤/分身登场、生命归零和恢复、玩家换位/移除随小步骤更新。
8. 每队可选择一位玩家的阵容；按单位筛选、只看关键变化、跳到生命归零/登场/变身等事件。播放和上/下一步遵循当前列表筛选。
9. 九宫格不显示号位文字，官方阵型坐标和朝向保持不变；每个单位在自己的格子中显示当前Buff名单及隐藏标记，悬停查官方说明，点击查完整配置。格内名单可滚动，同格单位可切换。
10. 默认展开双九宫格、收起右侧记录面板；名字、生命、护盾、气势固定，Buff 独立滚动。同格单位用菜单切换，单位/Buff 详情在战场内展开。
11. 有依据的关系从来源蓝点直达目标橙箭头，取消绕行折线；多目标分别指向对应格，自作用显示短环。逐条播放保留方向光点和目标脉冲，支持关闭动态提示及 0.5/1/2/4 倍展示速度。
12. 原始生命、护盾、气势增减值直接显示在各自色条内；同一条有多次变化时保留顺序，溢出菜单仍列出全部记录。点击数字或 Buff 增删回到对应小步；新增 Buff 绿色、移除记录红色，当前集合另列。未记录施加者时不补造连线。记录/原文按钮可展开完整侧栏。
13. 格子显示所选小步结算后状态，作用列表保留该小步内各条原始记录前后值。“显示解析依据”展开组序及路径；原文和全部字段一直保留。
14. 当前行动者格子放大 6.5% 并突出蓝框，基础阵型不移动；已登场且生命归零的当前显示单位整格变灰暗，恢复生命或切换到同格存活单位后恢复。
15. 免疫、格挡、被暴击、未命中、技能未生效和反击标记在目标格内分别显示，只依据原始目标字段。生命变化为 0 不推断免疫；技能未生效不抹去前后置数值及 Buff 变化。
16. Buff 的颜色区分施加来源：我方施加蓝色、敌方施加红色、来源未记灰色；不按持有者或当前行动者推断。初始 bfs 与 bd_bfs/ad_bfs 三段元组没有施加者字段，现有两场实战因此显示未知。成员逐条保留来源，同 ID 不同来源分行计数，移除首个匹配成员并保留其原施加来源，不把移除执行者当成施加者。
17. 技能在战场上方固定横幅用大字显示，标明施放阵营，点击查看官方说明；实际播放技能按官方内测替换、组合技和队伍执行分支读取，原始 si 单独保留，反击不冒用原进攻技能。连续播放时同一次行动的目标、数值及 Buff 记录一起展示；生命/气势条宽平滑过渡，文字一直是日志实际值，动态提示可关闭。
18. 属性强弱效、神职及符合条件的天启提示使用命中前的当前属性与原型资料，在对应格内用浅紫标签显示；与回包直接判定分开，准备/施放阶段不提前出现。
19. 追击本体、变身来源独立保存并在格内显示，不以本体种族或 groupRaceId 覆盖当前精灵原名。小神龙名单存在时显示准备、出手中、已完成，按已记录 eps[0] 定位且回退重建。
20. 展开 Buff 可查看隐藏附着动画、动画仅播一次、原版隐藏飘字等规则；表现规则不改变当前成员和条内数值。

阵位显示（2026-10-08 修正）：
日志 cts.x 是上下索引，cts.y 是前后排索引；格号 = x + y*3 + 1。
        我方（左侧）        敌方（右侧）
        7  4  1             1  4  7
        8  5  2             2  5  8
        9  6  3             3  6  9
双方 1/2/3 号位为前排，朝向中间。切换我方时按新的显示侧重新摆放，
不改日志坐标、身份或格号；同格多单位仍保留。原始坐标可在格子提示和全部字段中查看。
本次按用户要求重读 22/奥奇传说_阵容与布阵_总汇.md §1 和
22/奥奇传说_未验证项_解析补遗.md §2.3、§2.6，并核对官方定位器资源。
官方 battlebg/default 版本 20260930468207480 的 SingalLocator_default：
t0/p0 的 f0、f3、f6 屏幕横坐标依次为 226.05、126.05、26.05；
t1/p0 对应为 26.05、130.05、231.65。两个方向相反，前排都在靠敌侧。
完整 18 个格子的实测坐标、下载 URL 和 SHA-256 见 formation-evidence.json。
核对方法：JPEXS 26.2.1 -swf2xml 导出新下载的 battlebg_default.swf，
沿 SymbolClass(mmo.battle.SingalLocator_default) → t0/t1 → p0 → f0..f8
读取 PlaceObject2 的 matrix.translateX/Y，单位 twips 除以 20 得到像素。
修正前把 x/y 当作屏幕横纵坐标是错误的；此前两边均显示 1 2 3 / 4 5 6 / 7 8 9 的截图作废。

状态重建（2026-10-08 更新）：
护盾从初始 shp 累计客户端实际执行的变化；ashp 保留但不覆盖余额。
护盾 setter 不截断负值，不按生命上限封顶；初始缺失或变化损坏则显示待核实。
当前 Buff 按初始 bfs 和 bd_bfs/ad_bfs 维护；重复增加、仅删除一个匹配项。
队伍初始 Buff 包含 ppbf.pbid 与 pcbfs。隐藏 Buff 同样可查；记录次数不自动解释为层数。
旧 ba/bc/aa/ac_bfs 在当前官方命令中没有消费调用，保留原文并提示，不重复应用。
普通技能按各对象变化组的序号同步推进；playOrderType=1 时才按官方规则补空组。
有等待型新增 Buff 时，整组先处理显示 Buff；否则先处理生命/护盾，再气势/Buff。
每组数值及 Buff 命令之后处理 rl；技能后达到对象最后一组时，先校准 trl、再校准 cn，之后才处理 rl。
t=2 生命/上限重设可以覆盖同组的 ahp；prl 不参与最终 ahp/avg 校准。
攻击不能、无生效目标时不消费技能阶段数值；isse=false 仍处理前后置副作用。
近身单个生效目标 issb=true 时，在技能与后置阶段之间消费反击 sb 数值。
全部状态快照可逆访问，不通过“反向加减”猜测回退状态。
21 份官方反编译来源的哈希、方法定位与规则说明见 state-evidence.json。

数值条及战场状态（2026-10-08 本轮更新）：
生命为当前生命/生命上限；护盾按用户要求只显示当前余额。
气势分母由当前种族的官方 PetDictionaryDataItem.maxPower 给出：ultSuperSkillNo>0 时150，否则100。
不能统一假设100。文字保留超过分母的数值，绘制宽度最多填满；缺少字典时显示?。
新增9,722条官方精灵字典及属性配置；精灵包来源版本、原包哈希和反编译哈希随字典保存。
t=1 更新形态、外观、普通技能、当前超杀和属性；cu只消费首项，首项为0时取scu。
官方没有在此更新原suskill字段，因此保留原额外超杀字段。t=6只改变当前属性。
t=5按记录承载者的玩家及ci定位分身；不按未被此处理函数消费的ti/pn跨玩家定位。
明确召唤技能和分身指令保留各自的登场阶段；t=3本身不产生登场。2026-10-13纠正：等待型Buff的summonAppear只恢复显示层，以Effect.restoresDisplayLayers记录，不再写entered或新增登场事件。
entered代表已得到登场指令；本工具不模拟动画alpha、显示层和逐帧可见性。
生命归零和从零恢复仅表述数值事实，不补写导致这些变化的技能/被动因果。
玩家ml更新整套阵容的x_to/y_to，不更改精灵的x/y。dl后的阵容标明“已移除（末次记录）”。
11份新增核对来源、方法定位、包版本与规则见 battlefield-evidence.json。

仍有明确边界：
t=3 不单独改变登场状态，以官方召唤技能/等待型Buff阶段为准。
动画中的多个目标可能并行，特效还会随机分配气泡数值；文字版合并为同阶段结算，保留原始数组，不编造逐帧先后。
技能说明来自下载时的官方版本，不能据此断言历史日志所用的机制。
未覆盖：动画逐帧可见性、历史版本配置一致性核验、日志未给出的因果。
“全部原始信息可查”不等于“全部机制已解释”。

从零复现参考数据（在仓库根目录运行）：
python -X utf8 tools/battle_replay/audit_samples.py C:\keqi\config64\brush build-battle-reference/sample-audit.json
python -X utf8 tools/battle_replay/fetch_reference.py build-battle-reference

如需重新核对消费逻辑，用 JPEXS 对下载的 library_interfaces.swf、battle_battle.swf
导出 mmo.interfaces.battle.++、mmo.interfaces.battleconfig.++ 和 mmo.battle.++。
导出目录分别为 build-battle-reference/interfaces 和 build-battle-reference/battle。
导出只用于核对官方消费行为；不执行 SWF 或反编译脚本。

生成内置字典：
先导出 library_interfaces.swf 中 mmo.interfaces.pet.data.PetDictionaryDataItem 到 build-battle-reference/pet-interface。
python -X utf8 tools/battle_replay/fetch_pet_reference.py build-battle-reference --java <java.exe> --ffdec <ffdec.jar>
python -X utf8 tools/battle_replay/fetch_formation_reference.py build-battle-reference --java <java.exe> --ffdec <ffdec.jar>
最新表使用独立快照，避免覆盖较早的状态核对证据：
python -X utf8 tools/battle_replay/fetch_reference.py build-battle-reference/latest-tables
用 JPEXS 对该目录的 battleconfig_battleconfig.swf 导出 mmo.battleconfig.++ 到 latest-tables/battleconfig，
对 library_interfaces.swf 导出 mmo.interfaces.battleconfig.++ 到 latest-tables/interfaces。
python -X utf8 tools/battle_replay/build_catalog.py build-battle-reference/latest-tables assets/battle-replay-catalog.json --auxiliary-source build-battle-reference
--auxiliary-source 仅复用精灵、元能与布阵数据；必须与新 manifest 版本及接口包哈希吻合，否则生成失败。
来源 URL、manifest 版本、包与 XML 的哈希保留在字典 provenance 中。
本次核对所用反编译文本的哈希保留在 decompiledSourceSha256 中。

完整 Buff / 技能表连接（2026-10-08 最新官方 manifest 核对）：
Flash 版本链在 2026-10-09 补充修正：start.v 只指定 versiondata 包，资源版本取 versiondata 基础表、
再叠加 start.u.f 更新；两表均无条目时，按 VersManager 回退到 2000。不能把 start.v 当所有资源的版本。
H5 原包由 start/version 清单指定，均保存原始字节及 SHA-256。
21 份 Flash DefineBinaryData 原始载荷与 H5 battleconfig.aqz 的全部 21 个成员均保留。
Buff 有 34,520 行配置、30,339 行描述；按同 ID 连接，重复 ID 由最后整行覆盖，不拼接新旧行。
保留 34,486 个源键，其中 34,485 个是数值 ID，另一个原始键 jia 单独标记，绝不转换成 0。
did 是显示资源编号，不能据此借用其他 Buff 的名称或说明。无描述行可采用配置行的原有名称，来源明确标注。
最新表仍有 3,134 个无名称条目；400025/400026 等隐藏条目在 Flash 和 H5 都没有名称，保留空值。
技能文本来自官方 SkillConfigAdapter 及四张 SkillConfigSimple MAP，静态读取，不执行 AS。
21,155 个技能条目：18,306 个直接配置、268 个按官方 copyDefine 继承的配置、2,581 个仅说明条目。
派生技能使用来源配置的播放类型，自身说明元数据单独保留；描述存在不能据此伪造播放配置。
Flash/H5 的 20,777 条技能正文及五项描述元数据一致，原始格式差异保留。
独立 last 配置目前为空；last="1" 属性不等于存在独立 last 行。
附加效果载荷 SkillEmbedEffectXML 原文包含非法 XML 注释及两处缺失的属性分隔空格。
原始 13,739 字节完整保留；兼容结构视图仅移除注释和补两处空格，逐位置记录，不冒充原始 XML。
完整解出 38 条效果、13 条命令、8 段对话；30 条 sf 引用涉及 26 个技能，全部能连接。
sf 是表现播放的资格过滤，不表示技能施加 Buff；H5 的附加效果独立保存。

618 条明确的表引用双向连接：189 条已有 Buff 对应的播放替换，422 条队伍 Buff 的普通执行技能、7 条闪光执行技能。
来源 XML 行、XPath、字段、优先级与消费方法都可查；不把这些引用解释成“某技能实际施加了某 Buff”。
单位详情中的技能/Buff 名称支持点击，关联详情可双向跳转。
完整原表、解码字段、Flash/H5 差异和逐条连接记录在 build-battle-reference/latest-tables/linked：
  index.json：全部导出文件的 SHA-256 与官方来源。
  buffs.json / skills.json：完整连接结果，含源字段和来源。
  raw-buff-tables.json / raw-skill-tables.json：所有原始行、重复键、H5 原文及消费者证据。
  skill-embedded-effects.json：技能附加效果、对话及完整原字节/兼容编辑记录。
  relations.json：明确引用及双向索引；join-report.json：缺失、差异和解析统计。
内置 catalog 去除重复文字及逐行重复来源，完整数据仍在上述导出文件中，二者不混作原表。

检查真实日志引用覆盖：
python -X utf8 tools/battle_replay/audit_table_coverage.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/latest-tables/linked/log-coverage.json
只扫描官方已确认的显式 ID 字段，分别报告未收录、官方无名称、无说明。
包括玩家 hs 英雄技能和 abs 所需资源列表，后者不能当成当前 Buff；旧增删字段单列为当前命令未消费的引用。
0/-1 无对应官方条目时单列占位；-2 是官方有名称的 Buff，不当占位，也不通过 ID 符号猜测其他含义。

跨包复核和词条补充（2026-10-09）：
不能把“mess 为空”说成“没有任何官方解释”。本批缺 mess 的 253 个 Buff 中：
121 个有 beffect 效果备注；9 个可通过执行配置读取关联通灵技能说明；
9 个名称中的方括号词条可在 EntryConfigXML 中查到解释，另一个 [凡骨] 未找到，保持未解析。
词条解释按官方精确名称/中点参数规则查询，不据此声称某个 Buff ID 等同于该词条全部机制。
共导出 248 个官方词条，Flash/H5 一致；原模板和“指定数值”占位保留，不擅自填倍率。
UI 直接展示效果备注、关联技能说明和关联词条；不再把缺少独立说明泛化为官方完全没有信息。
原有名称、mess 和动作重建字段没有改写。
跨包取证目录：build-battle-reference/cross-resource-audit-20261009。
其中 local22 保存本地文本复核，other-packages 保存 SWF 静态扫描，consumer 保存读取链证据，
flash-resolved 保存按完整版本链取得的原包；flash-extra 是首轮 URL 留档，不作为实际版本索引依据。
90 个 H5 配置请求成功、2 个 schema URL 为 404；pet/petskillmesservice 的正确版本 URL 仍为 404。
限定结论为：本次已核对资源内未找到 27 个隐藏 Buff 的可靠名称映射，不能断言所有官方/服务器资源都没有。

保留原快照并生成补充字典：
python -X utf8 tools/battle_replay/build_catalog.py build-battle-reference/latest-tables assets/battle-replay-catalog.json --auxiliary-source build-battle-reference --exports build-battle-reference/cross-resource-audit-20261009/linked --verified-source build-battle-reference/cross-resource-audit-20261009
--verified-source 以最新完整版本链、重新下载的字节和哈希核对旧快照；仅版本标签相同不足以绕过变化。
词条完整表/引用保存在新 linked 目录的 official-entries.json、entry-mentions.json、raw-entry-tables.json。

关键依据（均在上述新下载的官方包）：
BattleResult.as：br、win、pve 的读取；pve 实际写入 replay，不能径直翻译为 PVE。
BattleUnit.as / RoundResult.as：fr/rl 顺序、普通行动及队伍行动分支。
FighterInfo.as：初始阵容、身份、召唤、分身、追击、生命、气势与护盾。
SkillResult.as：trl/prl、缺失 tci 时的队伍对象、命中/暴击/格挡/反击/生效标记。
SkillNumResult.as / BufferNumResult.as：数值字段、Buff 元组、变化组与阶段。
SkillNumUnitFactory.as 及对应子类：t=1..6、100、101 的原始结构。
BattleNumUnitUtil.as：t=4 是玩家的队伍进度，不是承载该记录的精灵 Buff 层数。
BattleUtil.as / BufferConsts.as：bd_bfs/ad_bfs 中 0=移除、1=出现；隐藏 Buff 仍保留。
AfterAttackCommand.as：最终生命/气势的写入以及没有 ashp 最终写入。

构建和检查：
在 PowerShell 中：& .\scripts\build.ps1 -Targets KQBattleReplayTest,KQBattleReplayPreview,KQPetInventory,KQWorkbenchUiPreview
ctest --test-dir build-v2 -R "battle_replay|domain_boundary|ui_boundary|workbench_ui_preview_smoke" --output-on-failure

设置 Qt bin 到 PATH 后：
KQBattleReplayPreview.exe --file <真实日志>
KQBattleReplayPreview.exe --audit <样本目录> <输出JSON>
KQBattleReplayPreview.exe --file <真实日志> --step 10 --capture <输出PNG>
KQBattleReplayPreview.exe --file <真实日志> --step 10 --moment 1 --capture <子步骤PNG>
KQBattleReplayPreview.exe --file <真实日志> --step 31 --effect first-hit --capture <作用PNG>

逐条作用独立核对：
KQBattleReplayPreview.exe --audit C:\keqi\config64\brush build-battle-reference/clarity-engine-audit.json
python -X utf8 tools/battle_replay/verify_effects.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/effect-audit-verification.json --cpp-report build-battle-reference/clarity-engine-audit.json
500份样本、66,255条行动：808,543条独立关系投影全部一致；2,377,567条Effect和619,421个小步的原文路径、前后数值、状态归属检查错误为0。
不同对象动画可能并行，作用列表不把显示顺序解释为官方逐帧先后。t4忠实使用prob/proa/vc；满格后的prob可能大于mp，不通过累加vc猜测隐藏进度。

精灵固有技能与气运／溯灵资料（2026-10-09）：
extract_pet_mechanisms.py 依据当前 PetDictionaryDataItem.create 参数声明读取精灵→技能、qiYunType 和 sign；
先核验源文件哈希，只有 update.timestamp > base.timestamp 才应用字典更新。
legendSkillNo 与 shenYunSkillNo 的官方别名关系保留；不通过种族编号拼接技能ID。
灵初优先选择溯灵，时代由 sign 精确标签判断。6685等官方缺方案保留未解析，不把1修正为11。
气运6项、溯灵8项原始规则及Flash/H5差异保留在 mechanismRules；这是客户端说明规则，不能代替服务器战斗计算。
旧传说技的 legendSkillNo 读取 formation.LegendSkillConfig，与神运技读取 BattleConfig 是不同命名空间。
legendMechanics 保留584个定义、3503个官方配置档。战报未记录传说石配置时展示各档，不按最高档补算；档序不等于等级。
Flash/H5传说技的非HTML字段与可见文字一致，HTML标记差异留在完整导出中，不宣称原文逐字相同。
单位详情可进入“官方机制资料”，再打开固有技能。没有开战能力值时不显示已激活或伪造动态数值。
原Buff名称、成员记录和战斗状态不由这些资料反推；本轮27个无名Buff仍不补写原因。

Grok独立核查后复核修正（2026-10-09）：
字典构造参数37在sign含“寰力量”或“异形族”时，由huanSkillNo getter读取，yuanLiSkillNo getter返回0；
NewSkillIconHelperV2对两类均调用BattleConfig.getHuanDefine，不能把这29条记成普通技能表缺口。
huanMechanics保留HuanConfigXML的15个ID、25个阶段行；29条精灵引用中28条命中，4832→704832仍未命中。
普通战斗技能未命中改为49条，连同1条寰力量配置缺口共50条；旧传说62条缺口保持不变。
HuanConfig是“寰力量／异族审判”，与TransformSkillDefine中type=2的幻元技分开处理。
单位机制资料显示完整条件和效果，不据当前精灵资料断言历史战斗已经激活。
Grok的原始输出及逐条复核保存在build-battle-reference/grok-supplement-audit-20261009；原始输出不直接作为官方证据。

重建本轮目录（版本链已核验）：
python -X utf8 -B tools/battle_replay/build_catalog.py build-battle-reference/latest-tables assets/battle-replay-catalog.json --auxiliary-source build-battle-reference --exports build-battle-reference/origin-link-audit-20261009/linked --verified-source build-battle-reference/cross-resource-audit-20261009 --mechanism-rules build-battle-reference/origin-link-audit-20261009/fortune/rules.json --legend-mechanics build-battle-reference/origin-link-audit-20261009/fortune/legend-mechanics.json --skin-source build-battle-reference/pet-identity-20261009/skin
--mechanism-rules 为本轮官方静态提取结果，按各方案source文件哈希校验，完整来源见 origin-link-audit-20261009/fortune/report.json。
不传该参数仍导出精灵技能引用，但气运／溯灵方案不视为已解析。
--legend-mechanics 为官方传说技独立表的完整导出，同样校验源文件哈希。完整选档代码和Flash/H5逐项核对在 fortune/legend-mechanics.json 中。

精灵原名（2026-10-09）：
--skin-source 指向当前换肤包的脚本导出目录。生成器现在要求此参数和 --verified-source，旧命令须补齐。
从 manifest 已校验的 changepetskinv3/changepetskinservice 解出 Cpsv3SkinConfig、Cpsv3SkinSummary、Cpsv3Skin、Cpsv3AvailablePet。
extract_pet_identity.py 核对 findOriginalRaceId/findRaceIdNeed 读取方，按 availablePets.raceId → raceIdNeed 精确关联，排除融合海报。
字典的 groupRaceId 仅保留归属证据：它也包含追击形态，不能直接改成该单位的原名。
9722条字典记录中，6448条原生名称、2545条皮肤原名对应、729条待核实；缺项、循环和歧义都不猜测。
6528慵懒浮梦对应5990[神运]虚构之神·奈非利塔；7405同名皮肤对应7403[灵初]虚构神权·奈非利塔。
7384鹤隐异灵未在精确换肤表命中；只确认groupRaceId=7355，不把它直接命名为本体或7356追击者。
九宫格、行动对象和机制资料使用原名；皮肤和自定义名留在详情，原始战报字节保留。
分身的oRaceId原精灵与raceId当前形态分别显示。技能、气势、属性与其他机制仍读取战报种族，绝不换成原精灵的机制编号。
完整对应链、来源哈希和缺口保存在 linked/pet-identities.json；运行目录保留petIdentitySummary与各条pets.identity。

独立核对每条行动的护盾与 Buff（先用上述 --audit 生成 state-engine-audit.json）：
python -X utf8 tools/battle_replay/verify_state_ledger.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/state-engine-audit.json build-battle-reference/state-ledger-verification.json
该脚本直接读取原始日志和官方字典，独立累计护盾及 Buff 成员数；逐行动比较全部单位状态摘要，不读取 C++ 中间事件。
本轮 500 份、66,255 条行动全部一致；包含 215,060 次 Buff 变更、25,886 条前后置护盾变化、40,116 条技能护盾变化、12 次反击。
619,421 个子步骤快照；真实样本的行动末护盾未知、负护盾和 Buff 集合不完整计数均为 0。

独立核对每个步骤的形态、技能、属性、气势分母、登场和玩家状态：
KQBattleReplayPreview.exe --audit C:\keqi\config64\brush build-battle-reference/battlefield-engine-audit.json
python -X utf8 tools/battle_replay/verify_battlefield_ledger.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/battlefield-engine-audit.json build-battle-reference/battlefield-ledger-verification.json
500份、66,255条行动和全部战斗段一致：3,844次变身、20,672次属性变化、994次旧实现标为等待型Buff登场的记录（此解释已于2026-10-13撤销；不得当作召唤次数）、192次分身呼出、543条玩家移除。
满气势分母缺失计数为0。本批没有ml玩家换位和独立召唤技能路径；用合成测试验证，不能声称得到真实案例覆盖。
回放界面测试包括大数值/超分母/未知条、敌我视角、同格单位、多玩家选择、单位筛选和跳到动作内部生命归零/恢复。

离线读取，不修改样本，不要求登录游戏。独立预览不写正式工作台偏好。

召唤和元能边框依据（2026-10-08）：
ics 是官方 isSummoned；ias/ibs 为分身，icr 为追击，不能混用。
ibat 没有找到官方客户端读取方，也出现在幻元自己变属性的单位上，不能直接翻译为元能目标。
宠物字典新增 nativeAttributes（字符串数组，保留官方未知占位）和 transformSkillNo（整数）。
独立 transformSkills 表来自当前 Flash formation/formationservice，不覆盖 battle skill type：
type/transformType 都保持官方构造参数的0元素、1赋能、2幻元，effectArr/autoEffectArr原样保留。

python -X utf8 tools/battle_replay/audit_empowerment.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/empowerment-audit.json
仅当同玩家有唯一非自身元能来源、唯一匹配当前种族属性差异，且单个元素效果精确一致、
目标非分身/追击/幻元/初始变身、原始ibat=true作附加佐证时，标为“官方配置与日志唯一匹配”。
官方SELF_RACE_TYPE/SELF效果可解释且结果与元素效果不同的独立幻元变化先分开核对；
同属性结果无法区分，仍不推定。官方Formation先执行自身幻元，再执行所选元素效果。
这是交叉核对，不能宣称已经解出了ibat语义。纯赋能和有歧义的记录不补写归属。
本批1,000个玩家中393个唯一匹配、91个有歧义、516个证据不足；涉及317份样本。
20260924194155-3-win.txt 的己方永夜君王·极对应决战终局·光明王（7号位），
敌方[灵初]至元御寰·极对应厨魂激燃·诺亚（2号位，亦为召唤单位）。
来源哈希、官方代码定位、具体样本和反例见 border-evidence.json。

只重核开局元能规则（不重复解析全部行动）：
KQBattleReplayPreview.exe --audit-empowerment C:\keqi\config64\brush build-battle-reference/empowerment-native-audit.json
python -X utf8 tools/battle_replay/audit_empowerment.py C:\keqi\config64\brush assets/battle-replay-catalog.json build-battle-reference/empowerment-verification.json --cpp-report build-battle-reference/empowerment-native-audit.json
本轮500份样本的输入SHA-256、来源/目标/技能ID与C++结果全部一致，差异0；
共存规则新增142个匹配，幻元结果与元素技相同的情况仍保持有歧义。


官方阶段连续播放（2026-10-09）：
Moment.phase / part / groupIndex / actor 保存各阶段边界和快照，原始数值、原文路径始终保留。
groupIndex 是各对象分组的同步游标，不跨对象排序原始 changeset。
页面保留官方回调边界：同组生命/护盾完成后，再展示气势/普通 Buff；等待型 Buff、状态操作、变化校准保持独立。
等待 Buff 按 Flash BattleUtil 的对象局部队列处理：wait2 串行、wait1/3 并行，末尾非等待项保留回调前后边界。
Effect 的 buffWaitType/buffPlaybackBatch/afterWaitCallback 保存局部关系，wait-tail 明示跨对象回调与数值动画的精确先后未记录，不能当全局帧序。
wait3 的 show-up 只标显现指令，不改 HP 或宣称复活；没有混入 H5 displayId 645 的独立提前回调特例。
出手前保持原快照，目标判定在技能作用阶段显示；反击单独切换行动者和箭头方向。
统一 QVariantAnimation 完成回调推进阶段和下一行动；暂停保留当前进度，倍速不跳过记录。
数字显示原始余额和变化量，仅数值条宽度插值；不仿造官方随机拆分的每击飘字。
文字版各阶段时长是界面展示节奏，不声称复原原版特效帧数或播放时长。
顶部进度条累计整次行动，不在每个内部阶段归零；缩短准备和普通变化停留，有明确生命归零/恢复、登场或变身的阶段延长阅读时间。全部原有阶段与快照保持可查。
格内 Buff 先列当前阶段的新增、移除和原记录明确关联的 Buff 作用，再按已记录来源整理当前集合；不借当前成员推断某次作用的施加者。
数值记录的非正关联编号不另标成 Buff 触发；官方播放链也使用负数作为数字显示标记，护盾链另取固定 ShieldBuffId。原始编号与数值变化仍保留。
“全部 / 收起”只展开该格内列表，展开自动暂停；同一战报回退和切换同格单位保留展开偏好，成功打开另一战报清空偏好。
紧凑显示不删日志：完整成员、未知/隐藏 Buff、来源、原始事件与说明仍可查看。数量是成员记录数，不表示服务端层数。
TYPE_ATTACK_DISABLE 不生成施放/命中动画；缺少官方配置时保留顺序待核实。
RepresenterSimpleAttackCommand 对应队伍触发只执行前后结算，原始目标判定不被补画成一次攻击。
行动抬头使用组合替换后的实际技能、反击者当时的普通技能，或 BufferExecute 确认的队伍技能；原始 si 单独保留。
队伍小神龙技能按 eps[0]、drs.di 及官方 ge 前两字符 aa 的闪光判定选取 lightedSkillId；无法匹配时不猜技能。
双方行动队列按本战斗段 pan/pbn 解析，缺字段继承，明确空数组显示空，null/格式错误保持未知；不推算未记的下一次行动。
双方轮次显示官方队伍轮次槽位；反击不借用原发起者的 fbs。队伍 Buff、英雄技、队伍目标判定直接放在阵容下方。
通灵进度按 Buff ID 独立保留，条内显示原始 prob/proa/vc；余额归零不等于施放。只有已记录的 isse 或匹配进度 Buff 的队伍技能显示生效/施放。
t=2 的 old/new 及原始差值与状态账本分别保存；条内优先显示 new-old，不把它误算成新值减当前快照。
气势达到已知上限只标“满”，不推断可用技能；当前属性、变身/技能替换、同格其余单位状态及确定的官方 Buff 效果词保留在格内。
本轮按用户范围排除开战道具结果 ir 和联盟兵工厂 ea.1uw；不把这些分支声称为已复刻。
组队容器 tbt 非 null 时按官方选择 brs 中最后含阵容 or 的子战报；选中项缺 fr 明确报错，不偷偷回退，也不把成员结果串成连续多局。全部成员原文与真实路径保留。
多条非空战线使用官方三槽计划器：队头阻塞不越过、玩家变动为屏障、每批各线一条行动，队列历史仍归属原 BattleUnit。
每条线从共同批次起点分线查看，批末合并互不冲突的单位；跨线读写同一单位的未记先后保留为未知，不猜执行次序。文字版不声称复刻跨线特效帧时序。
Sequential 逐节点服务的网络外层字段尚未由官方调用点确认，不能把任意数组或 tbt/brs 猜成这种模式。
现有 500 份存档和两场好友切磋均只有一条非空战线；多线与组队容器使用官方规则合成用例验证，未冒充真实并发样本验证。

预览验收（使用已编译程序）：
KQBattleReplayPreview.exe --file 战报路径 --verify-playback
KQBattleReplayPreview.exe --file 战报路径 --step 行动序号 --capture-stages 输出目录
KQBattleReplayPreview.exe --file 战报路径 --step 行动序号 --stage 阶段序号 --capture-board 输出.png
--verify-playback 无真实等待地跑完全部阶段，检查快照、行动覆盖、播放终点和原文保持。
--capture-stages 同时保存各阶段画面和带起止 moment 的 stages.json。

普通对战播放补齐（2026-10-09）：
本轮排除组队/多线扩展、战后奖励、剧情及嵌入事件；原有解析分支保持，不新增这些功能。
clientPresentation 保存官方 RaceTypeConfigXML 的 ID→列索引、XML 实际强弱阈值、属性优先级、忽略克制标记、PetJob 和当前原型资料，并保留来源哈希。
强弱效与神职提示按 PlayEffectCommand / PlayRepresenterEffectCommand 的真实分支生成；反击使用反击者及反转后的数值记录，不继承原攻击的忽略克制标记。
有效 tn 对象即使无数值变化仍保留命中边界以展示官方提示；没有 tn 或 isse=false 不造命中记录。
末击 isLastRound 来自交战段最后行动且无目标反击。已提供的末击配置按官方分支执行；本次显式配置为空，带 last 标记而未取得专用配置时显示待核实并保留最终记录观察。
内测形态严格依赖官方 desc==内测 及 9999 替换；日志种族、si、原名不被覆盖。初始 shp 完全缺省使用官方默认0，显式null与非法值保持未知。
等待 Buff 的 visible-or-skip / force-show 保存官方条件；战报无alpha，不能从生命归零或待召唤猜动画透明度。
小神龙 drs 使用官方 PetDragonDefines 名称/品质及 nv 参数化说明；仅 eps[0] 匹配该玩家实例ID且队伍技能出手分支已确认时记录 START。dbfi 相同不能代替实例匹配，后续 eps 不凭空追加出手。FINISHED 按官方当前席位和回合去重处理，simple 只有完成回调而没有 START；出手依据与队列完成依据分别保存。
文字版继续按阅读节奏连续播放，不生成原版随机拆分的飘字，也不声称复原历史动画帧和本地过滤设置。

普通回放易读性与接入补齐（2026-10-09）：
显式 tci=2147483647 与 null/缺省统一到官方队伍对象。初始阵容中找不到的实例引用只进入 Step.unlinkedReferences，保留玩家、队伍、原编号、判定、数值和完整路径，不创建虚假 Fighter。已有两场好友切磋里的 3275 属于这种引用，不能据 tpi2 把它放进某个阵容格。
重复 HP 变化组号与近身多有效目标等官方执行异常显示明确诊断；未确认的内部顺序不补演，仍保留最终绝对观察和原文。KeyEvent 按各 Effect 的记录顺序保留阶段内归零、恢复、变身、上限变化。同阶段首尾相同不再让中间变化消失。
每个同格单位有独立名称条、生命/在场状态、行动/受击标记及关系端点；点名称条切换详细数值和 Buff。格内有其他活着单位时，仅压暗阵亡单位。连续可确认的生命/气势链按共享进度播放条宽并短暂停顿，精确文字与原始变化量保持不变；任何断链、未知或上限改变都会回退，不能用条宽插值产生新数字。
clientPresentation.buffHarmSources 来自 BufferConsts.BUFF_HARM_SOURCE 的43项白名单；formations 来自 FormationImageConfig/FormationDefine 的21个阵型名称。脚本与当前固定哈希 SWF 重新导出核对，sourceEvidence 包含包与脚本 SHA-256、导出清单及重建命令。阵型属性参数不作为本场数值加成。Buff 的 #pet# 使用当前实际原名替换，未知施加者仍不染成任一方来源。
配置与版本、本步核对、阵容资料、完整队伍效果和完整行动队列均在画布内打开。当前数值的 ? 可查看来源字段、原值与未知原因，t=2 的生命/上限准确定位 new_curv/new_maxv。队列展开包括增减/改序与本轮已播放出手；受限或未确认执行另列，不把移出队列解释成死亡。
正式工作台 BattleReplaySource 只接收已有被动通道中的完整普通战报，并监听授权 brush 目录的稳定 txt/json 文件。文件修改须大小/时间稳定且两次内容摘要一致；原子替换和目录重建会恢复监听。初次目录中的旧文件作为基线，仍可手动导入。收到战报后固定原字节并按 SHA-256 去重，待阅容量为32份/64MiB，超限有可见提示；不会自动换掉当前文件或中断播放。
登录昵称来自官方 info.nn，info.n 只用于与已接纳本会话账号/递增 epoch 交叉核对。可靠昵称在战报中唯一匹配时才自动选我方。只读会话是否接纳与游戏写操作的 sourceVerified 权限无关；缺少昵称、会话失效及歧义都保留手动视角。源模块不发包、不启动新抓包、不更改样本文件。

真实时间播放验收（保留动画，以真实 QEventLoop/QTimer 运行）：
KQBattleReplayPreview.exe --file 战报路径 --verify-realtime-playback --playback-report 输出.json
该模式抽取最后两个普通/召唤行动连续播放到结局，核对共享进度、行动放大、暂停/改速/继续、最终快照和原文字节。它不是全场真实时长，也不是原游戏帧时序复刻。原 --verify-playback 仍覆盖全场全部阶段；--verify 另含同格、多目标、瞬时归零恢复、新战报不抢播和未知值入口等合成回归。

本轮最终验收（2026-10-09）：
build-battle-reference/goal-completion-20261009/validation.json 汇总10项完成内容、15组全部通过的针对性测试、二进制与目录哈希及证据边界；完成与验证.txt 为中文说明。
500份存档（66,255条行动）与改动前状态/事件摘要完全一致；另两份好友战报共154条行动，全部效果检查通过。未登记实例3275的2次/8次引用原文独立保留。
两场完整播放分别覆盖572/638个阶段；最新 first-realtime.json、rematch-realtime.json 均通过真实事件循环、暂停与倍速验证。同阶段100→0→100合成用例使用官方逐项消费的行动者数值链，不将远程目标合并值误当逐项中间值。
最终测试以 ui-final-test.xml、final-domain-and-integration-tests.xml 为准；同目录较早失败记录仅保留排查历史。
正式工作台被动接入已完成编译与会话/目录适配器验证；本轮未重新加载正在运行的游戏客户端做真实登录和新战斗现场验收，不能以离线测试代替该项证据。

观战聚焦调整（2026-10-09）：
施法条含精灵显示名、官方技能分类和技能名，改为浮在行动精灵所在行上方，随敌我视角与反击者切换；保留点击说明、暂停和共享进度。格间预留提示空间，行动格放大16%并加粗边框；直连箭头加粗，长箭头尖增至23px，同格短箭头仍约束在名称条侧边。
当前形态缺少精确原名映射时，仅在本局 originRelation/originFighterKey/originPath 均已确认且来源有精确原名时，显示“来源原名（追击体/变身后）”。不改变种族、技能、数值、catalog.identity 或全局去皮肤对应；详情保留当前官方形态名和来源路径。
两份好友战报中的7384由本局7383追击关联确定本体为[灵初]恒规秩序·龙尊；7332由同实例7331变身记录确定来源为[灵初]万灵神冕·究。7384→7356、7332→7320仍不宣称已经确认。无本局来源时继续显示原名待核实。
验收位于 build-battle-reference/visual-emphasis-20261009：ui-tests.xml 中界面和工作台测试通过，边界检查修正后以 boundary-final.xml 为准；realtime.json 通过真实事件循环的阶段、暂停、改速检查。上/中/下排及变身名称截图已人工检查。

Buff 折叠与回合提示（2026-10-09）：
每个精灵的当前 Buff 集合默认完全收起，仅留数量和展开按钮；点击后在本格显示全部已记录成员、来源、说明与当前形态信息。隐藏动画和未收录条目仍保留，展开会暂停播放。
格内自动提示仅列已经播到的本回合 Buff 获得、移除和明确触发记录，按战报 csn 分界；pr 是队伍行动轮次，不用于清空这些提示。进入下一回合清除提示，不改变真实当前集合。回合缺失时只显示本次记录，不推断持续时长。
历史提示保存行动序号和效果索引，点击回到该条原始记录；同一组可逐条查看，重复条数不表示层数。后退、跳步和视角变化均从当前播放位置重新建立提示，不泄漏未来记录，也不重放旧数值变化或箭头。施加方仍只使用明确来源，未知来源保持中性色。
箭头按用户提供的参考形状改为一体实心箭身、大三角尖端和白边。长距离为单条浅弧，短距离与同格关系按空间收窄，自施法使用本格回环；端点跟随行动格放大，动画仍使用共享播放进度。顶部回合选择同步实际播放回合。
验收位于 build-battle-reference/buff-fold-ribbon-20261009：ui-tests.xml 包含界面、UI 边界、工作台烟雾测试；realtime.json 为真实战报的事件循环验证。initial-collapsed、received-this-round、new-round、long-arrow 等截图记录本轮效果。

敌我关系与效果图形（2026-10-09）：
箭头颜色相对于已记录施加者和目标的实际队伍：对手红色，队友/自身绿色，队伍未知保持灰色；与当前我方视角和 Buff 施加来源配色分别处理。属性强弱效匹配每一对 sourceKey/targetKey，使用已确认官方属性名组合“神草强效”“神水增强强效”等，不生成官方没有的“神弱效”。长箭头就近附着标签，短箭头在目标上方连接提示，并避开数值、名字和技能条。
格内效果层提供区别化图标、短标题与共享进度动画，包含闪避、免疫、格挡/暴击、未生效、行动限制、反击标记/实际反击、归零/恢复、登场/分身、变身/属性变化、技能替换和已确认的官方客户端提示。多个效果保留图标并可点击切换；同格精灵的名称行独立保留各自图标。Buff 展开时隐藏图形层，让出完整列表；数值条不被覆盖。关闭动态提示仍可看静态标记，暂停不会留下独立动画。
目标大图标仅在普通攻击和队伍技能的 trl 结果分支显示。遵循 PlayEffectCommand / PlayRepresenterEffectCommand 的“闪避优先，其次免疫，再分别显示格挡和暴击”分支；完整原始 flags 仍保留。禁出手、单纯队伍触发、召唤、不确定流程和 prl 队伍代表者结果不补造命中动画。isse=false 的“未生效”属于文字版阅读提示，不宣称是原版素材；issb 与实际反击分别处理。
新增 Buff 的状态/属性图标只读取本阶段已记录事件和官方 beffect 白名单，不从常驻集合或上回合提示重新播放。“暴击↑”“闪避↓”“格挡↑”是属性升降，不能当作本次暴击/闪避/格挡。烧伤等来源标注仅在实际数值事件有已确认 bhs 时显示。图形为文字回放的辅助表现，不替换战报数字、不推导服务端计算。
验收位于 build-battle-reference/outcome-visuals-20261009：validation.json 汇总最终结果与证据；ui-final.log 是最终界面测试，realtime.json 是真实好友战报连续播放验证。qa/relation-outcomes.png 为明确标记的合成多目标场景；real-step12、real-step14 为真实战报的逐阶段截图，已检查红色敌对箭头、弱效、神水强效、同格追击者及阵亡效果。

十一项九宫格效果补齐（2026-10-10）：
1. 异常状态：核对官方 ID、did、名称、beffect 和说明后显示燃烧、冰冻、中毒、束缚、失明、物攻封印、晕眩、麻痹。新增状态不代表已经造成伤害，也不据失明推断本次闪避。
2. Buff 操作：本阶段明确移除、触发分别显示退场/脉冲图形；一条移除不等于清除全部同类成员。Buff 来源与效果类别分别显示，未知来源不补施加者。
3. 技能替换：本阶段特殊记录中普通技、超杀分别显示具体旧名→新名；纯换技不贴变身。
4. 形态和属性：显示已记录前后原名和属性；不把皮肤名当实际原名。未知映射保持待核，不从变化推断原因。
5. 元能/幻元：开局展示证据唯一的元能来源→目标，以及独立自身幻元；注明“原生→开局”，不演成战斗中施法。元能旧判据未放宽。
6. 伤害标注：仅数值事件已核对 bhs 时显示火焰、冻伤、中毒、吸血、反弹等区别图形，不凭 Buff 名称创造伤害或反向施法关系。
7. 特殊 Buff：战魂、不灭意志、已确认印记词及潜能爆发使用独立图形。印记 *1/*2/*3 取官方词，不把重复成员条数自动解释为层数。
8. 客户端提示：神职攻击、神职受击、天启分别显示；来源仍是已核官方客户端条件，区别于回包直接判定。
9. 满气势：已知气势跨越已知满值时突出提示，保留超额及准确数值，不推断下一次一定施放超杀。
10. 通灵：依据 vc、执行配置、prob/proa、isse 和当前已记录施法区分充能、扣除、生效/施放及一般变化；仅满槽归零不认定释放。缺 isse 按官方默认 false，显式异常值不猜。
11. 队伍来源：当前具体小神龙或执行 Buff 使用战场内来源卡，箭头从该卡出发。只将 eps 首项与当前玩家的唯一实例匹配，不从后项或同 dbfi 补猜。

同时修正：阶段尾显示最后一次已记录生命结果（例如100→0→100强调恢复），多个重要类别同时显示，不只剩1/N；完整Buff列表展开后仍保留紧凑结果区，不覆盖数值条。同格单位保持各自身份和提示。所有动画继续使用共享播放进度，暂停、回退及关闭动效不改变战报状态。
实现采用本地已解包并保留来源的官方资料，不表示重新下载核验了今日线上包。新 UI 合成用例使用明确标记的测试过程与官方定义，真实战报仍为此前两场好友切磋。没有新增组队扩展、奖励、剧情、嵌入事件、开战道具或联盟兵工厂。
本轮验收：build-battle-reference/eleven-effects-20261010/validation.json；最终界面、边界与工作台检查见 ui-verified.xml，其余域层检查见 tests.xml（其中早期 UI 失败已由最终报告覆盖）。realtime-final.json 核对实际事件循环、动态提示和暂停，rematch-all-stages.json 核对第二场84次行动、638个阶段及记录一致性。qa/ 下为明确标记的合成效果截图，real-critical.png / real-tianqi.png 为真实战报画面；长名称前后箭头及 Buff 来源角标另经截图检查。

Buff 记录按用途展示（2026-10-13 复核修正）：
旧版 2014 项固定“纯动画”名单已经撤销。原先按名称和空说明发现候选、再固定签名，只能证明元数据一致，不能证明没有状态或执行用途；此前相关分类报告是历史记录，不作为当前语义依据。
现在有三个独立入口：有官方说明的登记、未解释记录、播放详情。说明依据 mess 或官方 BattleAvatar 实际消费的 beffect 词表；不把 nm、did、hd、once、bhs 或素材是否空白当成真假 Buff 判据。有说明不等于已验证服务端公式。
原始成员按准确 ID 和已记录来源保留；同名、共用素材或动画编号不合并成状态层数。未解释登记保留原文和逐条跳转；当前或本回合有未知记录时有可见的折叠入口。来源未知保持未知。
实际数值触发、epbi 执行、显示控制按每条原始记录独立展示。同一 ID 可以同时承担多种用途。200195 的 team-simple 执行及目标前后结算保留；42383～42388 的六个编号分别保留，不推断层数；72899 仅有名称，不据此证明无法行动。
30098 对应的空白素材已经检查，但新增且 buffWaitType>0 并满足召唤/已呼出分身条件时，宿主仍调用 summonAppear。这是恢复子显示层的指令，不改整体 alpha、生命或召唤标志。域层只记 restoresDisplayLayers=true，格子显示“恢复显示”，不能生成复活或把该通用调用升级为新登场；HP为0和未知时仍按官方调用门槛处理。
隐藏、一次播放、等待类型和回调帧是独立的播放设置，放入播放详情；不以动画结束删除 Buff，不以帧号替代持续回合。暂停、跳转、切换视角不改变记录或来源。
证据：build-battle-reference/buff-semantic-audit-20261013/，含当前核心包、64个素材/74个脚本的静态检查、21份真实战报与17953条可回指原文的证据。2014项配置与消费者逐项核查；实际样本覆盖60项，其余1954项没有当前样本。原名单没有任何项在本次被证明“仅表现”，也不能反向称全部有战斗效果。
当前回归与安装记录：build-v2/validation/buff-roles-20261013/。旧头文件仅保存在该目录的 superseded-playback-whitelist.h，保留审计依据，禁止重新接入生产分类。


2026-10-13 用户界面与原始资料分层
正常卡片、技能横幅、通灵、行动队列、精灵/技能/Buff/阵容概览隐藏内部编号、代码字段、路径和等待帧数；未知名称保留“原名待确认”“未命名效果”等明确占位。不是删除数据，也不以名字重新分类效果。
replayPublicReferenceName 与 replayPublicFighterNameText 仅是 UI 投影，domain referenceName/petName、战报与官方配置不变；仅清理程序生成的未知原名占位，不通用删除数字或方括号，保留[灵初]、技能描述数值等真实名称/内容。已验证追击/变身原名关系继续使用。
常规详情通过 unit-raw / skill-raw / buff-raw / team-raw 链接打开原始资料，source-raw 查看接收来源与散列。相同显示名的不同编号仍是独立 href、完整 fighter key 与来源分组，重复计数仍是成员记录数。原始 JSON、历史路径、数值依据、等待/回调配置保留在显式原始详情及内部 evidence 属性；正常 tooltip 和读屏文本同样采用可读摘要。
技术配置、诊断、原始资料库收纳到“资料与原文”；原始记录面板不改。界面统一浅色背景、圆角控件和蓝色播放按钮，卡片维持数值红/蓝/橙、来源蓝/红、召唤绿、元能粉和死亡暗化规则。未改状态解析、生命/护盾/气势、通灵判定、原始输入或 Buff 正面说明依据。
本次验收目录：build-v2/validation/replay-public-ui-20261013；最终绑定检查目录：build-v2/validation/replay-public-ui-final-20261013。


2026-10-13 解析范围与质量检查
新增 audit_source_coverage.py：依据解析器显式 XML/H5/已审核 AS 契约清点输入，区分明确消费、仅原样保留与未知字段，并记录历史/本地证据、结构、SHA 及前后变化。审计本身不证明线上新鲜度或解析成功。
新增 public_catalog_quality.py：比较候选与现用逐条内容及字段退化，重算明确引用、源端点和正反向索引；精灵/技能两类更新共用此检查。已有官方缺项不猜填，不以两种字典的条数相等为验收。
回放在 publicUpdate 元数据固定后审核，再检查官方版本稳定和压缩包原子替换；未变化路径同样重新审计。报告绑定实际候选规范 JSON 散列，存 data-tools/quality/battleReplay-latest-attempt.json。拒绝仍保存报告并保留现用资料。
经实际数据骨架校验的 AS 字节变更保存收据；未知消费者代码、未识别新增字段与原有已消费字段丢失不自动放行。首次导出发现不等于官方新增加，只有准确匹配批准生产包 SHA 或逐项输入证据才降为既有覆盖信息。
这轮完成覆盖与质量门槛，不宣称全部未知资源已理解；持续解析适配、部分历史 H5 证据刷新仍需按官方消费者继续补齐。详情见 docs/manual-public-data-updater.md；验收目录 build-v2/validation/parser-quality-20261013。

皮肤模块本轮核对：当前官方包与历史包的33个AS中3个变化，包含两个非回放消费的海报/分享UI类，故不宣称全包仅数据变动。原名解析消费的5类字节不变；Cpsv3SkinConfig两个已核实字面量数组（SKIN_DEFINES_SRC、SKIN_GROUPS_SRC）以严格形状与外围字节相等为持续更新契约。原始官方URL、SHA、全脚本及差异见 qa/skin-review。
