"""Compare independent Python inventory with native replay output, per file."""
import argparse
import collections
import json
from pathlib import Path


def verify(inventory, engine):
    source = {item['file']: item for item in inventory['files']}
    actual = {item['file']: item for item in engine['files']}
    if inventory['errors'] or engine['failed']:
        raise ValueError('input or engine reported failures')
    if source.keys() != actual.keys():
        raise ValueError('file sets differ')
    for name in source:
        for field in ('sha256', 'actions', 'nodes'):
            if source[name][field] != actual[name][field]:
                raise ValueError(f'{name}: {field} differs')
    return {'files': len(actual), 'actions': sum(row['actions'] for row in actual.values()),
            'nodes': sum(row['nodes'] for row in actual.values()), 'engineMs': engine['elapsedMs']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('inventory', type=Path)
    parser.add_argument('engine', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--state-report', type=Path)
    parser.add_argument('--battlefield-report', type=Path)
    args = parser.parse_args()
    read = lambda path: json.loads(path.read_text(encoding='utf-8-sig'))
    inventory, engine, catalog = read(args.inventory), read(args.engine), read(args.catalog)
    result = verify(inventory, engine)
    state_check = read(args.state_report) if args.state_report else None
    battlefield_check = read(args.battlefield_report) if args.battlefield_report else None
    if state_check and (state_check['mismatches'] or state_check['files'] != result['files'] or state_check['counts']['actions'] != result['actions']):
        raise ValueError('state ledger verification differs')
    if battlefield_check and (battlefield_check['mismatches'] or battlefield_check['files'] != result['files'] or battlefield_check['actions'] != result['actions']):
        raise ValueError('battlefield ledger verification differs')
    skill_ids, buff_ids = collections.Counter(), collections.Counter()
    for entry in inventory['files']:
        root = read(Path(inventory['sourceDirectory']) / entry['file'])
        battle = root.get('br', root)
        for block in battle['fr']:
            for action in block.get('rl', []):
                if 'si' in action:
                    skill_ids[str(action['si'])] += 1
                if 'epbi' in action:
                    buff_ids[str(action['epbi'])] += 1
    missing_skills = {key: count for key, count in skill_ids.items() if not catalog['skills'].get(key, {}).get('name')}
    missing_buffs = {key: count for key, count in buff_ids.items() if not catalog['buffs'].get(key, {}).get('nm')}
    lines = [
        '战斗回放：数值条与战场状态核对结果',
        '',
        '已完成生命红条、护盾蓝条、气势橙条，条内显示完整的当前值/参考值。',
        '已接入动作子步骤、护盾余额、当前 Buff、形态/属性/技能、召唤/分身登场及玩家换位/移除状态。',
        '支持玩家阵容选择、单位筛选、关键变化过滤和跳到事件对应的小步骤。',
        '实现依据：brush 原始日志 + 新下载的奥奇传说官方包；没有采用 22 中的旧方案。',
        '',
        '阵位更正（2026-10-08）：按用户要求重读 22 中的描述，并重新下载官方 battlebg/default 核对 18 个定位点。',
        '此前把日志 x/y 当成屏幕横纵坐标是错误的；x 为上下索引，y 为前后排索引。',
        '我方：7 4 1 / 8 5 2 / 9 6 3；敌方：1 4 7 / 2 5 8 / 3 6 9。双方前排朝向中间。',
        '修正只改变显示映射，原始阵位、单位身份和回放数据保持不变。来源记录见 tools/battle_replay/formation-evidence.json。',
        '',
        f"逐文件交叉检查：{result['files']} 份；{result['actions']:,} 条行动；{result['nodes']:,} 个原始 JSON 节点。",
        'Python 独立扫描与 C++ 解析输出的文件集合、每份 SHA-256、行动数、节点数全部一致。',
        f"C++ 全量读取耗时：{result['engineMs'] / 1000:.2f} 秒。原始字段保留不代表语义全部完成。",
        f"规范化结构路径：{len(inventory['schema'])} 种（包含容器）。初始单位记录：{inventory['counts']['fighters']:,} 条。",
        '',
        '已核实并处理的重点：',
        '同格单位、跨队伍同 ID 不合并；队伍对象与精灵分开；未记录的命中标记不补成未命中。',
        'isse=false 的 tn 仍保留；无伤害的行动仍进入时间线；重复 Buff 不去重。',
        '气势不擅自封顶为 100；ashp=0 不擅自清空护盾；pve 不翻译为 PVE/PVP。',
        't=4 的进度记录挂到所属玩家的队伍对象；pr.dl 作为玩家移除记录单独显示。',
        '当前 Buff 从初始列表及 bd_bfs/ad_bfs 重建，保留隐藏和重复项，每次删除一个匹配记录；不把次数自动解释为层数。',
        '每组按照等待型 Buff、生命/护盾、气势、最终校准及特殊记录的官方命令规则推进，技能和反击阶段独立显示。',
        '护盾按实际执行变化累计；技能后先进行 ahp/avg 校准，再执行该组 rl，所以 t=2 重设不会被错误覆盖。',
        '护盾条分母是生命上限，气势条分母是当前种族的官方满气势值（100或150）；两者都不限制分子的真实数值。',
        '变身保留单位身份和日志名称，另显示当前形态；同步更新普通技能、cu首项（0时用scu）、属性和满气势。',
        't=3本身不触发登场；等待型Buff、召唤技能及t=5分身指令分别按官方消费阶段处理。',
        '玩家移动只更新整套阵容位置，精灵格位仍用各自x/y；已移除的阵容标明末次记录。',
        ('独立状态核对：' + json.dumps(state_check, ensure_ascii=False)) if state_check else '当前报告未附独立状态核对结果。',
        ('独立战场状态核对：' + json.dumps(battlefield_check, ensure_ascii=False)) if battlefield_check else '当前报告未附独立战场状态核对结果。',
        f"子步骤总数：{sum(row.get('moments', 0) for row in engine['files']):,}；依据见 tools/battle_replay/state-evidence.json。",
        f"满气势依据缺失计数：{sum(row.get('unknownPowerReferenceObservations', 0) for row in engine['files'])}；本批有待核实提示的文件数：{sum(bool(row['issues']) for row in engine['files'])}。",
        '条比例、形态和登场等新依据见 tools/battle_replay/battlefield-evidence.json。',
        '本批真实样本没有玩家ml换位或独立召唤技能路径；这些分支使用合成记录测试，不声称已获真实案例覆盖。',
        '',
        '仍需验证的边界：',
        '1. 大体型视觉占位未查证，本版按日志x/y格位显示，不推断扩占邻格。',
        '2. 未模拟动画alpha、显示层及逐帧时刻；同阶段数值汇总不冒充动画命中段数。',
        '3. 当前官方包晚于历史日志，不能直接认定历史版本机制完全相同。',
        '4. 数值前后变化不能证明某个被动是其原因；未来未知类型保留原文，不补造解释。',
        '',
        '当前字典覆盖（仅统计行动 si 和队伍触发 epbi，不冒充全部 Buff 覆盖）：',
        f'行动技能 ID：{len(skill_ids)} 种；缺官方文字名称：{len(missing_skills)} 种。',
        '缺名称的技能 ID 与出现次数：' + json.dumps(missing_skills, ensure_ascii=False),
        f'队伍触发 Buff ID：{len(buff_ids)} 种；缺官方文字名称：{len(missing_buffs)} 种。',
        '缺名称的队伍 Buff ID 与出现次数：' + json.dumps(missing_buffs, ensure_ascii=False),
        '',
        '样本覆盖计数：',
        json.dumps(inventory['counts'], ensure_ascii=False, indent=2),
        '',
        '优先人工核对的三份样本（独立覆盖扫描选出）：',
        *[item['file'] + '：' + ', '.join(item['adds']) for item in inventory['representatives']],
        '',
        '官方来源：',
        *[f"{p['resource']}  版本 {p['version']}\n{p['url']}\nSHA-256 {p['sha256']}" for p in catalog['provenance']['packages']],
        *[f"{p['resource']}  版本 {p['version']}\n{p['url']}\nSHA-256 {p['sha256']}" for p in catalog['provenance'].get('pets', {}).get('packages', [])],
        'H5 battleconfig 版本：' + catalog['provenance']['h5']['version'],
        '',
        '代码与复现步骤：KQPetInventoryExtension/tools/battle_replay/README.txt',
        '独立预览：双击同目录的“战斗回放预览.cmd”。工作台源码入口为“战斗回放”（Ctrl+5）。',
        '已构建独立预览和插件 DLL；现有客户端里的插件没有被替换。',
    ]
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
