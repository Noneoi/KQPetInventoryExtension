#include "public_data_detail_query.h"
#include "public_data_view.h"
#include "public_skill_view.h"
#include "pet_metadata_view.h"
#include "compiled_shop_catalog.h"
#include <QJsonArray>
#include <QSet>
#include <QRegularExpression>
#include <cmath>

namespace {
QString scalar(const QJsonValue& value) {
  if (value.isString()) return value.toString();
  if (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() == std::floor(value.toDouble()))
    return QString::number(value.toInteger());
  return {};
}
QString identifier(const QJsonValue& value) {
  const auto id = scalar(value);
  return id == QStringLiteral("0") || id == QStringLiteral("-1") ? QString() : id;
}
QJsonObject root(const PublicDataDetailContext& context, PublicDataViewKind view) {
  if (context.replay) return view == PublicDataViewKind::BattleReplay ? context.replay->root : QJsonObject{};
  const auto& d = context.dataset;
  if (!d) return {};
  switch (view) {
    case PublicDataViewKind::PetDetail: return d->detail ? d->detail->root : QJsonObject{};
    case PublicDataViewKind::PetSkills: return d->skill ? d->skill->root : QJsonObject{};
    case PublicDataViewKind::BattleReplay: return d->replay ? d->replay->root : QJsonObject{};
    case PublicDataViewKind::Shop: return d->shop ? d->shop->root : QJsonObject{};
    case PublicDataViewKind::Routine: return d->routine ? d->routine->root : QJsonObject{};
    case PublicDataViewKind::PetImages: return d->imageIndex;
    case PublicDataViewKind::Icons: return d->iconIndex;
  }
  return {};
}
QJsonObject arrayRows(const QJsonArray& values) {
  QJsonObject result;
  for (int i = 0; i < values.size(); ++i) result.insert(QString::number(i), values[i]);
  return result;
}
// Mirrors the existing reviewed replay projections; indexes remain in their
// original namespace and are not treated as a battle Buff/skill identifier.
QJsonValue table(const QJsonObject& data, const PublicRecordKey& key) {
  if (key.view==PublicDataViewKind::PetSkills && key.table==QStringLiteral("petStarGodSkills")) {
    const auto embedded=data.value("pets").toObject().value(key.id).toObject().value("starGodSkill");
    return embedded.isUndefined() ? QJsonObject{} : QJsonObject{{key.id,embedded}};
  }
  if (key.view != PublicDataViewKind::BattleReplay) return data.value(key.table);
  const auto supplement = data.value("replaySupplements").toObject();
  if (key.table == QStringLiteral("resources")) {
    QJsonObject rows;
    const auto groups = supplement.value(key.table).toObject();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
      const auto group = it.value().toObject(); auto metadata = group; metadata.remove("rows");
      rows.insert(it.key() + QStringLiteral("/config"), metadata);
      const auto values = group.value("rows").toArray();
      for (int i = 0; i < values.size(); ++i) rows.insert(it.key() + QLatin1Char('/') + QString::number(i), values[i]);
    }
    return rows;
  }
  if (supplement.contains(key.table)) {
    const auto group = supplement.value(key.table).toObject();
    auto rows = arrayRows(group.value("rows").toArray());
    if (key.table == QStringLiteral("titles")) { auto metadata = group; metadata.remove("rows"); rows.insert("config", metadata); }
    return rows;
  }
  const auto cultivation = data.value("replayCultivation").toObject();
  if (cultivation.contains(key.table)) return cultivation.value(key.table).toObject().value("definitions");
  if (key.table == QStringLiteral("legends")) return data.value("legendMechanics").toObject().value("definitions");
  if (key.table == QStringLiteral("petDragons")) return data.value(key.table).toObject().value("definitions");
  if (key.table == QStringLiteral("qiYun") || key.table == QStringLiteral("suLing"))
    return data.value("mechanismRules").toObject().value(key.table).toObject().value("plans");
  if (key.table == QStringLiteral("coverage")) return arrayRows(data.value("referenceCoverage").toObject().value("items").toArray());
  if (data.value(key.table).isArray()) return arrayRows(data.value(key.table).toArray());
  return data.value(key.table);
}
PublicRecordValue record(const PublicDataDetailContext& context, const PublicRecordKey& key) {
  return PublicDataView(key.view, QJsonObject{{key.table, table(root(context,key.view),key)}}).record(key);
}
QString named(const QJsonObject& row) {
  for (const auto* field : {"name","title","nm","label","heroSkillName","summonSkillName"}) {
    const auto value = row.value(QLatin1String(field)).toString();
    if (!value.trimmed().isEmpty()) return value;
  }
  return {};
}
QString originalName(const PublicDataDetailContext& context, const PublicRecordKey& key, const QJsonObject& row) {
  if (key.view == PublicDataViewKind::BattleReplay) {
    const auto identity = row.value("identity").toObject();
    const auto status = identity.value("status").toString();
    if (status == QStringLiteral("native") || status == QStringLiteral("skin"))
      return identity.value("canonicalName").toString();
    return {};
  }
  const auto metadata = root(context,PublicDataViewKind::PetDetail).value("pets").toObject();
  auto current = metadata.value(key.id).toObject();
  if (current.isEmpty()) return {};
  QString name = current.value("name").toString();
  QSet<QString> visited{key.id};
  while (true) {
    const auto parent=scalar(current.value("groupRaceId")); bool valid=false;
    const auto group=parent.toLongLong(&valid);
    if (!valid || group<0 || group>2147483647 || QString::number(group)!=parent) return {};
    if (group==0) return name;
    if (visited.contains(parent)) return {}; // Invalid loop cannot prove an original.
    visited.insert(parent); current = metadata.value(parent).toObject();
    if (current.isEmpty()) return {};
    name = current.value("name").toString();
  }
}
QString description(const QJsonValue& raw) {
  if (raw.isString()) return raw.toString();
  QStringList values;
  const auto row = raw.toObject();
  for (const auto* field : {"description","descX","desc","mess","beffect","tip","descriptionTemplate","autoEffectDescription","reasonLabel"}) {
    if (QLatin1String(field) == QLatin1String("desc") && !row.value("descX").toString().isEmpty()) continue;
    const auto text = row.value(QLatin1String(field)).toString();
    if (!text.isEmpty() && !values.contains(text)) values.append(text);
  }
  if (values.isEmpty()) {
    const auto texts = row.value("descriptions").toObject();
    for (auto it = texts.begin(); it != texts.end(); ++it)
      if (it.value().isString() && !it.value().toString().isEmpty()) values.append(it.value().toString());
  }
  return values.join(QLatin1Char('\n'));
}
void relation(PublicDataDetail& out, PublicDataViewKind view, const QString& tableName,
              const QString& id, const QString& label, const QString& evidence) {
  if (id.isEmpty() || tableName.isEmpty()) return;
  for (const auto& entry : out.relations)
    if (entry.key.view == view && entry.key.table == tableName && entry.key.id == id && entry.label == label) return;
  out.relations.append({{view,tableName,id},label,evidence});
}
void appendText(PublicDataDetail& out, const QJsonObject& row, const char* field, const QString& label) {
  const auto text = row.value(QLatin1String(field)).toString();
  if (!text.isEmpty()) out.fields.append({label,text});
}
QJsonObject source(const PublicDataDetailContext& context, const PublicRecordKey& key, const QJsonObject& data) {
  QJsonObject result{{"catalogSource",data.value("source")}};
  if (key.view==PublicDataViewKind::BattleReplay && data.value("provenance").isObject())
    result.insert("catalogProvenance",data.value("provenance"));
  if (context.replay) {
    result.insert("sha256",context.replay->sha256); result.insert("sourcePath",context.replay->sourcePath);
    result.insert("sourceLabel",context.replay->sourceLabel);
  } else if (context.dataset) {
    static const QStringList names{"pets","skills","battleReplay","shop","routines","images","icons"};
    const auto component = context.dataset->components.value(names.value(int(key.view)));
    result.insert("origin",component.origin); result.insert("sha256",component.sha256);
  }
  return result;
}
PublicDataDetail shopDetail(const PublicDataDetailContext& context, const PublicRecordKey& key) {
  PublicDataDetail out; out.key=key; out.kind=PublicDataDetailQuery::kindName(key); out.title=out.kind;
  const auto catalog = context.dataset && !context.replay ? context.dataset->shop : nullptr;
  if (!catalog || key.table != QStringLiteral("goods")) return out;
  const ShopExchangeGood* found=nullptr;
  const ShopExchangeShop* owner=nullptr;
  for (const auto& shop : catalog->allShops) for (const auto& good : shop.goods) if (good.stableKey()==key.id) {
    if (found) { out.state=PublicValueState::InvalidContainer; return out; }
    found=&good; owner=&shop;
  }
  if (!found) return out;
  const auto& good=*found; out.state=PublicValueState::Present; out.title=good.displayName(); out.description=good.description;
  out.revision=context.dataset->revision; out.source=source(context,key,catalog->root);
  out.fields.append({QStringLiteral("商店"),good.shopName});
  if (!good.costDescription.isEmpty()) out.fields.append({QStringLiteral("价格说明"),good.costDescription});
  const auto compiled=CompiledShopCatalog::compile({good});
  if (!compiled.goods().isEmpty() && compiled.goods().first().costCondition.effectiveState()==ShopConditionState::Satisfied) {
    QStringList prices;
    const PetMetadataView metadata(context.dataset->detail);
    for (const auto& item : compiled.goods().first().requirements) {
      const auto parts=item.resourceKey.split(':'); bool t=false,i=false;
      const int type=parts.value(0).toInt(&t), id=parts.value(1).toInt(&i);
      if (!t || !i || parts.size()!=2) continue;
      QString label;
      if ((type==4 || type==8) && context.dataset->detail) {
        const auto tableName=type==4 ? QStringLiteral("items") : QStringLiteral("money");
        label=context.dataset->detail->root.value(tableName).toObject().value(QString::number(id)).toObject().value("name").toString();
        relation(out,PublicDataViewKind::PetDetail,tableName,QString::number(id),QStringLiteral("消耗材料"),"strict compiled cost type/id");
      } else if (type==134 && (id==1 || id==2)) label=metadata.materialName(type,id);
      if (label.isEmpty()) label=QStringLiteral("材料名称未收录");
      prices.append(QStringLiteral("%1 ×%2").arg(label).arg(item.required));
    }
    if (!prices.isEmpty()) out.fields.append({QStringLiteral("所需材料"),prices.join(QStringLiteral(" ＋ "))});
    else if (good.provenFree) out.fields.append({QStringLiteral("所需材料"),QStringLiteral("无需资源（资料已明确）")});
  }
  if (good.provenUnlimited) out.conditions.append(QStringLiteral("资料已明确不限次数"));
  else if (good.limitCount>0) out.conditions.append(QStringLiteral("%1限 %2 次").arg(good.limitLabel).arg(good.limitCount));
  if (good.startsAt.isValid()) out.conditions.append(QStringLiteral("上架时间：%1").arg(good.startsAt.toString(Qt::ISODate)));
  if (good.endsAt.isValid()) out.conditions.append(QStringLiteral("结束时间：%1").arg(good.endsAt.toString(Qt::ISODate)));
  if (!good.unlock.isEmpty()) out.conditions.append(QStringLiteral("另有官方解锁条件；当前账号是否满足请看商店中的资格分析。"));
  for (int id : good.raceIds) relation(out,PublicDataViewKind::PetDetail,"pets",QString::number(id),QStringLiteral("适用精灵"),QStringLiteral("goods.raceIds"));
  out.source.insert("sourceUrl",good.sourceUrl);
  // Keep the original matching shop row, including all unrendered fields.
  for (const auto& value : catalog->root.value("shops").toArray()) {
    const auto row=value.toObject();
    if (row.value("shopId").toInt()==good.shopId && row.value("sourceKey").toString()==good.sourceKey) {
      // A shop may repeat an item ID in multiple variants. Retain its whole
      // original row rather than choosing an apparent first matching item.
      out.raw=row; break;
    }
  }
  if (out.raw.isUndefined()) out.raw=QJsonObject{{"sourceKey",owner->sourceKey},{"shopId",good.shopId},
      {"itemServerId",good.itemServerId},{"description",good.description},{"enhanceType",good.enhanceType},
      {"unlock",good.unlock},{"cost",good.cost},{"rewardRaw",good.rewardRaw}};
  return out;
}
}

QString PublicDataDetailQuery::kindName(const PublicRecordKey& key) {
  static const QMap<QString,QString> names{{"pets",QStringLiteral("精灵资料")},{"skills",QStringLiteral("技能资料")},
    {"buffs",QStringLiteral("效果资料")},{"entries",QStringLiteral("官方词条")},{"officialEntries",QStringLiteral("官方词条")},
    {"officialEntryMentions",QStringLiteral("官方词条")},{"goods",QStringLiteral("商店项目")},
    {"badges",QStringLiteral("元魂")},{"astrolabe",QStringLiteral("星轮")},{"stargods",QStringLiteral("星神")},
    {"sacredEquipment",QStringLiteral("源兽装备")},{"sourceBeasts",QStringLiteral("源兽")},
    {"legendStones",QStringLiteral("传说石")},{"proficiencies",QStringLiteral("潜能")},{"items",QStringLiteral("道具")},
    {"money",QStringLiteral("资源")},{"transformSkills",QStringLiteral("转化类技能")},{"petStarGodSkills",QStringLiteral("星神技")},
    {"summon",QStringLiteral("召唤配置")},{"carry",QStringLiteral("携同配置")},
    {"almighty",QStringLiteral("全能配置")},{"hero",QStringLiteral("英雄技配置")},{"psychics",QStringLiteral("通灵配置")}};
  return names.value(key.table,QStringLiteral("官方资料"));
}
QString PublicDataDetailQuery::title(const PublicDataDetailContext& context, const PublicRecordKey& key) {
  const auto value=record(context,key);
  const auto row=value.raw.toObject();
  if (key.table==QStringLiteral("pets")) {
    const auto name=originalName(context,key,row);
    return name.isEmpty() ? QStringLiteral("原名待核实") : name;
  }
  if (key.view==PublicDataViewKind::BattleReplay && key.table==QStringLiteral("buffs")) {
    const auto name=row.value("nm").toString().trimmed();
    return name.isEmpty() ? row.isEmpty() ? QStringLiteral("效果名称未收录") : QStringLiteral("未命名效果") : name;
  }
  const auto name=named(row);
  if (!name.isEmpty()) return name;
  if (key.table==QStringLiteral("entries") || key.table==QStringLiteral("officialEntries") || key.table==QStringLiteral("officialEntryMentions")) return key.id;
  return kindName(key);
}
PublicDataDetail PublicDataDetailQuery::resolve(const PublicDataDetailContext& context, const PublicRecordKey& key) {
  if (key.view==PublicDataViewKind::Shop) return shopDetail(context,key);
  PublicDataDetail out; out.key=key; out.kind=kindName(key); out.title=title(context,key);
  const auto data=root(context,key.view); const auto value=record(context,key); const auto row=value.raw.toObject();
  out.state=value.state; out.raw=value.raw; out.description=description(value.raw);
  out.replayReference=key.view==PublicDataViewKind::BattleReplay;
  out.revision=context.replay ? context.replay->sha256 : context.dataset ? context.dataset->revision : QString();
  out.source=source(context,key,data);
  if (key.view==PublicDataViewKind::PetSkills && key.table==QStringLiteral("entries") && value.state==PublicValueState::Missing) {
    const auto term=PublicSkillView(data).term(key.id);
    if (term.source==PublicSkillTermSource::ParameterizedOfficial) {
      out.state=PublicValueState::Present; out.description=term.description;
      out.raw=QJsonObject{{"requested",term.requested},{"entryName",term.entryName},
          {"arguments",QJsonArray::fromStringList(term.arguments)},{"definition",data.value("entries").toObject().value(term.entryName)}};
      out.source.insert("reading",QStringLiteral("EntryConfigParser / EntryArgLike")); return out;
    }
  }
  if (!value.exists()) return out;
  if (out.replayReference && key.table==QStringLiteral("officialEntryMentions") && row.value("status").toString()!=QStringLiteral("resolved")) {
    out.description.clear(); out.conditions.append(QStringLiteral("当前官方词条表未能确定这项说明，原文与核对记录已保留。"));
  }
  for (const auto* field : {"source","sourceUrl","sourcePath","sourceTable","referenceConsumption"})
    if (row.contains(QLatin1String(field))) out.source.insert(QLatin1String(field),row.value(QLatin1String(field)));
  if (key.table==QStringLiteral("pets")) {
    if (named(row)!=out.title) out.formName=named(row);
    appendText(out,row,"officialRole",QStringLiteral("官方定位"));
    appendText(out,row,"position",QStringLiteral("定位"));
    appendText(out,row,"quality",QStringLiteral("品质"));
    if (key.view==PublicDataViewKind::PetDetail) {
      const auto base=identifier(row.value("groupRaceId"));
      relation(out,key.view,"pets",base,QStringLiteral("原始精灵"),"PetDictionary.groupRaceId");
      // Both projections are explicitly built from the same PetDictionary race
      // key. This is not a general same-number cross-namespace fallback.
      if (!record(context,{PublicDataViewKind::PetSkills,"pets",key.id}).raw.isUndefined())
        relation(out,PublicDataViewKind::PetSkills,"pets",key.id,QStringLiteral("技能与官方配置"),"PetDictionary race key");
    } else if (key.view==PublicDataViewKind::PetSkills) {
      const PublicSkillView skills(data); const auto skillSlots=row.value("slots").toObject();
      for (const auto& slot : PublicSkillView::slotDefinitions())
        relation(out,key.view,"skills",identifier(skillSlots.value(slot.first)),skills.slotLabel(slot.first,row),"pets.slots."+slot.first);
      relation(out,key.view,"transformSkills",identifier(skillSlots.value("transform")),QStringLiteral("转化规则"),"pets.slots.transform");
      relation(out,key.view,"pets",identifier(row.value("evolutionFrom")),QStringLiteral("进化前精灵"),"pets.evolutionFrom");
      for (const auto& skin : row.value("skins").toArray())
        relation(out,key.view,"pets",identifier(skin.toObject().value("raceId")),QStringLiteral("皮肤 / 形态"),"pets.skins.raceId");
      for (const auto& entry : row.value("skillEntries").toArray())
        relation(out,key.view,"entries",entry.toString(),QStringLiteral("官方词条"),"pets.skillEntries");
      if (row.value("starGodSkill").isObject()) relation(out,key.view,"petStarGodSkills",key.id,QStringLiteral("星神技"),"pets.starGodSkill");
    } else if (out.replayReference) {
      const auto mechanics=data.value("petMechanics").toObject().value(key.id).toObject();
      for (const auto& value : mechanics.value("skillLinks").toArray()) {
        const auto link=value.toObject();
        relation(out,key.view,"skills",identifier(link.value("skillId")),link.value("label").toString(QStringLiteral("技能")),"petMechanics.skillLinks");
      }
    }
  }
  if (key.view==PublicDataViewKind::PetSkills && key.table==QStringLiteral("petStarGodSkills"))
    relation(out,key.view,"pets",key.id,QStringLiteral("对应精灵"),"pets.starGodSkill owner");
  if (key.view==PublicDataViewKind::PetSkills && key.table==QStringLiteral("entries")) {
    const auto term=PublicSkillView(data).term(key.id);
    if (term.hasOfficialEntry()) out.description=term.description;
  }
  if (key.view==PublicDataViewKind::PetSkills && (key.table==QStringLiteral("skills") || key.table==QStringLiteral("buffs"))) {
    if (key.table==QStringLiteral("skills")) for (const auto& value : data.value("combos").toObject().value(key.id).toArray()) {
      const auto combo=value.toObject();
      relation(out,key.view,"buffs",identifier(combo.value("buff")),QStringLiteral("切换所需状态"),"combos.buff");
      relation(out,key.view,"skills",identifier(combo.value("skill")),QStringLiteral("切换后的技能"),"combos.skill");
    }
    const auto view=PublicSkillView(data);
    auto matches=QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]")).globalMatch(out.description);
    while (matches.hasNext()) {
      const auto term=matches.next().captured(1); const auto found=view.term(term);
      if (found.source==PublicSkillTermSource::ExactOfficial)
        relation(out,key.view,"entries",term,QStringLiteral("原文词条"),"description bracket / exact EntryConfigParser key");
      else if (found.source==PublicSkillTermSource::ParameterizedOfficial)
        relation(out,key.view,"entries",term,QStringLiteral("原文参数词条"),"EntryArgLike entryName/arguments");
    }
  }
  if (out.replayReference) {
    const bool skill=key.table==QStringLiteral("skills"), buff=key.table==QStringLiteral("buffs");
    if (skill || buff) {
      QSet<QString> wanted;
      for (const auto& id : data.value(skill ? "relationsBySkill" : "relationsByBuff").toObject().value(key.id).toArray()) wanted.insert(id.toString());
      for (const auto& value : data.value("tableRelations").toArray()) {
        const auto linked=value.toObject(); if (!wanted.contains(linked.value("id").toString())) continue;
        const auto kind=linked.value("kind").toString();
        if (kind!=QStringLiteral("combo_buff_playback") && kind!=QStringLiteral("buffer_execute_skill") &&
            kind!=QStringLiteral("buffer_execute_lighted_skill")) continue;
        // Existing official tableRelations are the reviewed executable links;
        // candidates, raw same-number fields and name matches never enter here.
        relation(out,key.view,"skills",identifier(linked.value("skillId")),QStringLiteral("关联技能"),linked.value("sourcePath").toString());
        relation(out,key.view,"buffs",identifier(linked.value("buffId")),QStringLiteral("配置状态"),linked.value("sourcePath").toString());
        relation(out,key.view,"skills",identifier(linked.value("targetSkillId")),QStringLiteral("切换后的播放技能"),linked.value("sourcePath").toString());
      }
      for (const auto& id : data.value(skill ? "entryMentionsBySkill" : "entryMentionsByBuff").toObject().value(key.id).toArray())
        relation(out,key.view,"officialEntryMentions",id.toString(),QStringLiteral("原文词条"),"officialEntryMentions");
    }
    for (const auto* field : {"sourceRaceIds","targetRaceIds","carrierRaceId","carriedPetRaceId","raceId","psychicsCalledPetRaceId","skillId"}) {
      const auto v=row.value(QLatin1String(field));
      const auto values=v.isArray() ? v.toArray() : QJsonArray{v};
      for (const auto& id : values) relation(out,key.view,QLatin1String(field)==QLatin1String("skillId") ? "skills" : "pets",
          identifier(id),QLatin1String(field)==QLatin1String("skillId") ? QStringLiteral("配置技能") : QStringLiteral("配置精灵"),QString::fromLatin1(field));
    }
    if (key.table==QStringLiteral("powerFull")) relation(out,key.view,"buffs",identifier(row.value("buffId")),QStringLiteral("满气势表现配置"),"powerFull.buffId");
    if (row.contains("runtimeEligible") && !row.value("runtimeEligible").toBool()) out.conditions.append(QStringLiteral("原表有此记录，但未被当前官方读取分支选中。"));
  }
  if (key.view==PublicDataViewKind::PetDetail && key.table==QStringLiteral("sacredEquipment"))
    relation(out,key.view,"sourceBeasts",identifier(row.value("sourceId")),QStringLiteral("源兽定义"),"sacredEquipment.sourceId");
  if (key.view==PublicDataViewKind::PetDetail) {
    if (key.table==QStringLiteral("sourceBeasts")) appendText(out,row,"attr",QStringLiteral("属性"));
    const auto levels=row.value("levels").toObject();
    for (auto it=levels.begin();it!=levels.end();++it) {
      bool valid=false; const int level=it.key().toInt(&valid); const auto text=description(it.value());
      if (valid && level>=0 && QString::number(level)==it.key() && !text.isEmpty())
        out.fields.append({QStringLiteral("%1 级说明").arg(level),text});
    }
  }
  for (const auto* field : {"conditionDescription","teamDescription","conditionsText"}) {
    const auto text=row.value(QLatin1String(field)).toString(); if (!text.isEmpty()) out.conditions.append(text);
  }
  for (const auto* field : {"maxLevel","stargodSlotMaxLevel"}) {
    const auto n=scalar(row.value(QLatin1String(field)));
    if (!n.isEmpty()) out.fields.append({QLatin1String(field)==QLatin1String("maxLevel") ? QStringLiteral("等级上限") : QStringLiteral("星神槽等级上限"),n});
  }
  if (key.view==PublicDataViewKind::PetDetail && key.table==QStringLiteral("stargods")) {
    const auto jobs=root(context,key.view).value("jobs").toObject(); QStringList names;
    for (const auto& id : row.value("limitJobs").toArray()) { const auto name=jobs.value(scalar(id)).toString(); if (!name.isEmpty()) names.append(name); }
    if (!names.isEmpty()) out.conditions.append(QStringLiteral("适用职业：%1").arg(names.join(QStringLiteral("、"))));
  }
  return out;
}
