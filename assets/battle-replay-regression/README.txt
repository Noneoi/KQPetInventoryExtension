公共回放资料更新的真实战报回归夹具（2026-10-14）

用途
  KQBattleReplayUpdateCheck 是仅使用 Qt Core 的独立程序。它与工作台链接同一份
  BattleReplay 域层，以及相同的 brpack 解码器和 catalog 结构校验器；不创建 GUI，
  不执行下载的 SWF/JS，也不推演服务器伤害公式。每次候选资料发布前以冻结夹具检查。

来源与可信边界
  三例来自用户已有的真实战报：
    team-execution.json      <- 20260924193432-1-fail.txt  （55 次行动）
    summon-and-sequence.json <- 20260924194155-3-win.txt   （133 次行动）
    clone-entry.json         <- 20260924192435-86-win.txt  （148 次行动）
  原件位于 C:/keqi/config64/brush，维护过程只读。每例原件 SHA256、原件已验证投影
  SHA256、清理后 SHA256、清理后的已验证投影均在 manifest.json 中。
  不是新编阵容，不是合成战果；测试故意篡改的是候选资料的临时副本，绝不当成官方数据。

  基线使用上次已通过的真实更新成果：
    catalog JSON SHA256
      fcc3c720db523435c63000da2c3dc8300c18df7853ec37d27b18d62cdeeb0571
    已验证旧预览 EXE SHA256
      455b0153ef6bd1f77f8ddab50367aa61c72df7f6f47827bb0540dd7be4099a42
    核验记录
      build-v2/validation/parser-quality-20261013/corpus-validation.json
      build-v2/validation/native-update-regression-20261014/
  维护脚本先把原件投影与旧记录比对，再把清理前后投影按玩家匿名标识归一后逐项比对。
  三者经已登记玩家身份归一后的战斗数字、单位目标、技能、Buff、顺序与显示恢复记录必须相同。新的有序状态期望由
  这一固定旧基线上的原生观察产生，同时要求其旧投影哈希仍与旧预览完全一致。

隐私清理（manifest 逐路径记录操作，但不保存被删的私人字段值）
  1. 仅保留外层 br；删除刷图次数、请求结果、命令等 envelope 字段。
  2. 删除 br.bi（战斗实例编号）和 br.ir（本任务不做的开战装饰/道具结果）。
  3. 删除初始玩家记录里的 id、nn、items；删除精灵 customName。
  4. 初始玩家 name 及所有 cpn/tpn/pan/pbn/pn/player 引用改为 side0-player0 /
     side1-player0。空 pan/pbn 仍为空；不把缺失队列补成存在。
  5. cpi/cpn、tpi/tpn、pi/pn 成对的正玩家编号按同一匿名角色稳定替换为
     1000 + team.id * 100 + playerIndex；保留数值/字符串类型和非正哨兵。
     不对全局同值数字做替换，未配有 pn 的 pi 不作为玩家编号处理。
  6. 保留精灵原始游戏名称、战斗单位实例 ID、种族/外观 ID、全部非身份战斗数字及数组顺序、
     队伍 ID、技能/效果/目标/时机/变化组与其他战斗结构。JSON 仅统一编码和对象键序。
  清理过程不改动原文件。原始客户端昵称不随程序发布。

  privacySchema=2 补齐了旧清理中遗漏的动作数字玩家引用。manifest 的 privacyMigration
  只记录变更路径、操作、前后文件散列与等价性证明，不保存真实编号或编号映射表。
  迁移先验证原件、旧夹具和旧原生投影的既有散列，再逐值证明除了这些成对身份引用，
  其余键、类型、数组顺序、单位身份和数值完全相同。privacyRebaselinePending=true 时
  不得发布；使用同一个固定旧基线原生观察后才能冻结，状态、提示、统计和覆盖不得变化。

检查内容
  - 候选 JSON/brpack 的原始字节 SHA256、结构与包完整性；manifest 的 SHA256；
    每份夹具的 SHA256；报告绑定这些值，不能用另一个候选的结果替代。
  - BattleReplay::parse 无错误，原文和 root 保留，字段数量与原始投影保持一致。
  - 每个 effect 的原路径、原值、记录 before/after/delta、归属以及有序 moment
    拼接满足既有原生预览的独立 raw invariant 检查。
  - 每个行动的 moment、数值、Buff 多重集合及施加来源、形态/属性/技能、队伍进度、
    等待/显示恢复命令、目标、生死边界、队列/回合/战线语义与冻结有序投影一致。
  - 每个行动从其真正初始 observation 再执行 advanceAction，结果相同。
  - 使用 clientPresentationCues 检查官方客户端属性/神职提示的语义投影。
  - 倒序和非连续访问冻结状态并核对哈希。它验证域层快照，不声称验证了 GUI 播放动画。
  - 最终状态、每步 checkpoint 哈希与单独的有序投影哈希可定位失败范围。

已覆盖的真实情况
  普通出手、多目标、200195 队伍简单执行、30098 显示恢复、t=3 召唤记录、待召唤
  单位、t=5 分身实际登场、形态改变、生命归零/恢复、三种数值的增减与过程顺序。
  确切计数来自 manifest 中 expected.coverage，不由文件名推断。

没有覆盖或不能因此证明的事项
  - 这三例没有 confirmed summon-skill execution；召唤记录/显示恢复不能代替该分支。
  - 不是所有精灵、所有技能、所有机制的穷尽证明；未重新登录游戏进行实时新战斗。
  - 不校验 GUI 动画时长/位置，也不验证服务器伤害计算。
  - 用户排除的组队、剧情、奖励、开战道具不扩展进此回归范围。
  - 名称/说明可能合法变更，故不冻结为旧文字。报告保留实际 consumedDefinitionValues
    及其哈希；准确的编号到官方文字/配置关联由更新器的官方源保真检查负责。原生回归
    单独通过不等于文字关联正确；两项发布条件都必须通过。

维护
  工具 tools/battle_replay/build_regression_fixtures.py 不在公共更新链路内。
  准备模式必须显式给出 --verified-corpus、--baseline-catalog、
  --baseline-preview、--samples、--evidence、--qt-bin、--output，且所有基线哈希匹配。
  然后使用 KQBattleReplayUpdateCheck --catalog 已验证基线 --fixtures manifest.json
  --report baseline-observation.json --observe-baseline。该模式始终输出 passed=false，
  仅 observationComplete 表示内部审计完成，不能直接作为候选发布许可。
  最后维护脚本 --output ... --freeze-observation baseline-observation.json，必须再次
  匹配被固定的基线 catalog/manifest 哈希，且旧投影等价；不能拿更新候选自改期望。
  对既有 v1 夹具补齐数字身份时，先使用 --upgrade-numeric-identities --samples 原件目录
  --legacy-projections 已验证旧投影目录；此步骤仅运行 Python，完整保留旧基线约束。
  随后原生 --observe-baseline，再 --freeze-observation。冻结要求原生 effects 散列等于
  旧已核投影仅归一玩家身份后的预测散列；只允许身份相关的 effects/orderedProjection
  散列更新，不允许战斗数值、状态、提示、行动数量或覆盖变更。
  冻结后重新编译内嵌 qrc，再运行 native 集成测试。若合法官方更新确实改变旧战报的
  展示规则，需要人工查验官方消费者和数据关系，再专门审核基线变更，不能自动放行。

日常 CLI
  KQBattleReplayUpdateCheck --catalog 候选.brpack或json --report 报告.json
  默认使用程序内置夹具。成功退出 0，schemaVersion=1、passed=true；失败非 0，保留
  失败摘要和候选/夹具绑定。它只写指定报告，不覆盖候选或正在使用的资料。
