"""Semantic loss guards. Individual official additions/removals stay legal."""


def validate_semantic_transition(component, before, after):
    if component == 'skills':
        old_rules, new_rules = before.get('relationRules', {}), after.get('relationRules', {})
        for kind in ('summon', 'carry'):
            old, new = old_rules.get(kind, {}), new_rules.get(kind, {})
            if old.get('available') is True and (new_rules.get('schema') != 1 or new.get('available') is not True):
                raise ValueError(f'关系表 {kind} 从已解析退化为不可用，本次保留原资料')
            if old.get('bySourceRaceId') and not new.get('bySourceRaceId'):
                raise ValueError(f'关系表 {kind} 全部丢失，需要核对官方原始配置')
    if component == 'pets':
        for field in ('stargods', 'astrolabe', 'sacredStarPlans', 'sacredStagePlans', 'sourceBeasts'):
            if before.get(field) and not after.get(field):
                raise ValueError(f'养成规则 {field} 全部丢失，需要核对官方原始配置')
    if component == 'routines' and before.get('tasks') and not after.get('tasks'):
        raise ValueError('日常任务目录全部丢失，本次保留原资料')
    if component == 'shop':
        def records(value):
            return {(shop.get('sourceKey', ''), shop.get('shopId'), good.get('itemServerId', good.get('id'))): good
                    for shop in value.get('shops', []) for good in shop.get('goods', [])}
        prior = records(before)
        for key, row in records(after).items():
            old = prior.get(key, {})
            # A changed official reward can legitimately change semantics. A
            # still identical reward must not silently lose its known rule.
            if old.get('rewardRaw') and old.get('rewardRaw') == row.get('rewardRaw'):
                for field in ('enhanceType', 'raceIds'):
                    if old.get(field) and not row.get(field):
                        raise ValueError(f'商品 {key} 的官方奖励未变，但已解析的 {field} 丢失')
