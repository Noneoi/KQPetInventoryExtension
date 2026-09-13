#include "pet_power_calculator.h"
#include "checked_json_numbers.h"
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <limits>
#include <vector>

namespace {
bool integer(const QJsonValue& value, int* result, int minimum = 0) {
  qint64 number = 0;
  if (!DomainNumeric::checkedInteger(value,&number,minimum,std::numeric_limits<int>::max())) return false;
  *result = int(number); return true;
}
bool add(int& total, int value) {
  const qint64 next = qint64(total) + value;
  if (next < 0 || next > std::numeric_limits<int>::max()) return false;
  total = int(next); return true;
}
bool power(const PetPowerMetadata& m, int id, int level, int* value) {
  return id > 0 && level > 0 && integer(m.stargod(id).value(QStringLiteral("battlePower"))
      .toObject().value(QString::number(level)),value,1);
}
struct Slot { int index = 0, id = 0, source = 0, level = 0; bool changeable = false; };
struct Star { int id = 0, type = 0, quality = 0; bool movable = false, allowed = false; };
using Groups = QMap<int,QList<int>>;
bool starInfo(const PetPowerMetadata& m, int id, Star* star) {
  const auto entry = m.stargod(id); star->id = id;
  if (!integer(entry.value(QStringLiteral("quality")),&star->quality) || star->quality > 6 ||
      !entry.value(QStringLiteral("changeable")).isBool()) return false;
  star->movable = entry.value(QStringLiteral("changeable")).toBool();
  if (star->movable) { star->allowed = true; return true; }
  if (!integer(entry.value(QStringLiteral("type")),&star->type) || !entry.value(QStringLiteral("limitJobs")).isArray()) return false;
  star->allowed = true;
  // Star fragments and slot experience materials are not equippable stars.
  if (star->type >= 97 || star->quality < 2) { star->allowed = false; return true; }
  QSet<int> forbidden;
  for (const auto& value : entry.value(QStringLiteral("limitJobs")).toArray()) {
    int job = 0; if (!integer(value,&job)) return false; forbidden.insert(job);
  }
  if (!forbidden.isEmpty() && m.jobs.isEmpty()) return false;
  for (const auto& value : m.jobs) {
    int job = 0; if (!integer(value,&job)) return false;
    if (forbidden.contains(job)) star->allowed = false;
  }
  return true;
}
bool parseSlots(const QJsonValue& encoded, const PetPowerMetadata& m, QList<Slot>* parsedSlots) {
  if (!encoded.isString() || encoded.toString().size() > 4096) return false;
  if (encoded.toString().isEmpty()) return true;
  const auto sequences = encoded.toString().split(QLatin1Char('#'),Qt::KeepEmptyParts);
  if (sequences.size() > 64) return false;
  for (const auto& sequence : sequences) {
    const auto fields = sequence.split(QLatin1Char(':')); Slot slot; slot.index = int(parsedSlots->size());
    if (fields.size() == 1) {
      if (!integer(fields[0],&slot.id,-2) || slot.id > 0) return false;
      if (slot.id == -1) slot.id = 0;
      slot.level = 1;
    } else {
      if (fields.size() < 2 || fields.size() > 4 || !integer(fields[0],&slot.id,-2) || slot.id == -1 ||
          !integer(fields[1],&slot.level,1) || (fields.size() >= 3 && !integer(fields[2],&slot.source,-2))) return false;
      slot.source = qMax(0,slot.source);
    }
    slot.changeable = slot.id == -2 || m.stargod(slot.source).value(QStringLiteral("changeable")).toBool() ||
        m.stargod(slot.id).value(QStringLiteral("changeable")).toBool();
    parsedSlots->append(slot);
  }
  return true;
}
struct Assignment { QList<int> ids, values; int total = 0; bool known = true; };
// Maximum-weight matching gives each ordinary slot a distinct star type.
// Empty dummy columns preserve empty parsedSlots. Actual levels are never promoted.
Assignment assign(const QList<Slot>& parsedSlots, const Groups& groups, const PetPowerMetadata& m, int fixedLevel = 0) {
  Assignment result;
  const int n = int(parsedSlots.size()), g = int(groups.size()), columns = g+n;
  result.ids.fill(0,n); result.values.fill(0,n);
  if (!n) return result;
  if (g > 256) { result.known = false; return result; }
  std::vector<std::vector<qint64>> costs(n,std::vector<qint64>(columns));
  QList<QList<int>> choices;
  for (int i = 0; i < n; ++i) {
    QList<int> ids; ids.fill(0,columns); int j = 0;
    for (auto group = groups.begin(); group != groups.end(); ++group,++j) {
      int best = 0, bestId = 0;
      for (int id : group.value()) {
        int value = 0;
        if (!power(m,id,fixedLevel ? fixedLevel : parsedSlots[i].level,&value)) { result.known = false; continue; }
        if (value > best || (value == best && id == parsedSlots[i].id)) { best = value; bestId = id; }
      }
      ids[j] = bestId;
      costs[i][j] = -qint64(best)*1024 - (bestId && bestId == parsedSlots[i].id ? 1 : 0);
    }
    choices.append(ids);
  }
  std::vector<qint64> u(n+1),v(columns+1);
  std::vector<int> p(columns+1),way(columns+1);
  for (int i = 1; i <= n; ++i) {
    p[0] = i; int j0 = 0;
    std::vector<qint64> minv(columns+1,std::numeric_limits<qint64>::max());
    std::vector<bool> used(columns+1);
    do {
      used[j0] = true; const int i0 = p[j0]; int j1 = 0;
      qint64 delta = std::numeric_limits<qint64>::max();
      for (int j = 1; j <= columns; ++j) if (!used[j]) {
        const qint64 cur = costs[i0-1][j-1]-u[i0]-v[j];
        if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
        if (minv[j] < delta) { delta = minv[j]; j1 = j; }
      }
      for (int j = 0; j <= columns; ++j) {
        if (used[j]) { u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0]);
    do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
  }
  for (int j = 1; j <= columns; ++j) if (p[j]) {
    const int row = p[j]-1,id = choices[row][j-1]; int value = 0;
    if (id && !power(m,id,fixedLevel ? fixedLevel : parsedSlots[row].level,&value)) result.known = false;
    result.ids[row] = id; result.values[row] = value;
    if (!add(result.total,value)) result.known = false;
  }
  return result;
}
Assignment changeableAssignment(const Slot& slot, const QList<int>& ids, const PetPowerMetadata& m, int fixedLevel = 0) {
  Groups group; group.insert(0,ids); return assign({slot},group,m,fixedLevel);
}
void astrolabe(const QJsonObject& pet, const PetPowerMetadata& m, PetBattlePowerState& out) {
  const auto raw = pet.value(QStringLiteral("astrolabe"));
  const auto era = m.eraResolved || m.era != PetEra::Unknown ? m.era : resolvePetEra(pet);
  out.breakthroughApplicabilityKnown = era != PetEra::Unknown;
  out.breakthroughApplicable = eraHasAstrolabeBreakthrough(era);
  out.breakthroughKnown = out.breakthroughApplicable && pet.value(QStringLiteral("astrolabebr")).isBool();
  out.breakthrough = out.breakthroughKnown && pet.value(QStringLiteral("astrolabebr")).toBool();
  if (!raw.isString() || raw.toString().size() > 16384) return;
  out.astrolabeApplicabilityKnown = true; out.astrolabeApplicable = !raw.toString().isEmpty();
  if (!out.astrolabeApplicable) { out.astrolabePowerKnown = true; return; }
  int senior = 0,nodes = 0; bool known = true;
  for (const auto& chain : raw.toString().split(QLatin1Char('|'))) for (const auto& text : chain.split(QLatin1Char('#'))) {
    const auto fields = text.split(QLatin1Char(':')); int id = 0,light = 0,selected = 0,value = 0,located = 0;
    if (++nodes > 128 || fields.size() < 2 || fields.size() > 3 || !integer(fields[0],&id) ||
        !integer(fields[1],&light) || light > 1 || (fields.size() == 3 && (!integer(fields[2],&selected) || selected > 1)) ||
        (selected && !light)) { known = false; continue; }
    const auto node = m.astrolabe.value(QString::number(id)).toObject();
    if (!integer(node.value(QStringLiteral("battlePower")),&value) || !integer(node.value(QStringLiteral("locatedType")),&located) || located > 2) { known = false; continue; }
    if (located == 2) ++senior;
    if (!light) ++out.astrolabeUnlitNodes;
    if (light && selected) ++out.astrolabeSelectedNodes;
    if (!add(out.astrolabeMaximum,value) || (light && !add(out.astrolabeCurrent,value))) known = false;
  }
  out.astrolabeTargetBonus = out.breakthroughApplicable && senior >= 3 ? 150 : 0;
  out.astrolabeBonus = out.breakthroughApplicable && out.breakthrough && out.astrolabeSelectedNodes >= 3 ? 150 : 0;
  if (!add(out.astrolabeCurrent,out.astrolabeBonus)) known = false;
  out.astrolabePowerKnown = known && (out.astrolabeSelectedNodes < 3 ||
      (out.breakthroughApplicabilityKnown && (!out.breakthroughApplicable || out.breakthroughKnown)));
}
QString explanation(const QString& key) {
  if (key == QStringLiteral("lv")) return QStringLiteral("等级与种族基础成长；官方等级公式为等级×种族基础战力÷200，整数截断。");
  if (key == QStringLiteral("iv")) return QStringLiteral("有效天赋合计×3×当前等级÷200；有效属性由种族职业决定，含星能提升。当前使用本次详情的独立天赋分项。");
  if (key == QStringLiteral("pl")) return QStringLiteral("职业熟练度及通灵培养贡献；依据当前职业培养状态与官方目标比较。");
  if (key == QStringLiteral("ep")) return QStringLiteral("精灵装备槽与强化等级的战力贡献。");
  if (key == QStringLiteral("gsv")) return QStringLiteral("守护石战力贡献；与传说石单独统计。");
  if (key == QStringLiteral("lav")) return QStringLiteral("官方学习培养战力分项；不把未确认的 lp 属性串当作此分项。");
  if (key == QStringLiteral("lsv")) return QStringLiteral("传说石槽位、等级与传说技贡献。");
  if (key == QStringLiteral("bsv")) return QStringLiteral("各职业元魂等级与专属元魂觉醒贡献；依据当前/极限元魂分项，不把未觉醒当作已满。");
  if (key == QStringLiteral("asv")) return QStringLiteral("全部已点亮星轮节点贡献；只有灵初精灵在已突破且选中达到3个时额外增加150，其他时代不计突破加成。");
  if (key == QStringLiteral("sjv")) return QStringLiteral("神源兽星级、阶级及实际配置贡献；使用该实例当前/极限分项。");
  return QStringLiteral("官方详情返回的独立战力分项；缺少映射时保留原键，不合并或猜测。");
}
}

PetPowerMetadata petPowerMetadataFromCatalog(const QJsonObject& pet, int slotMax, const QJsonObject& stargods,
    const QJsonObject& astrolabeSheet, const QJsonObject& pets) {
  PetPowerMetadata result; result.stargods = stargods; result.astrolabe = astrolabeSheet; result.slotMaxLevel = slotMax;
  result.era = resolvePetEra(pet,pets); result.eraResolved = true;
  int race = 0,alternate = 0;
  if (!integer(pet.value(QStringLiteral("r")),&race,1)) integer(pet.value(QStringLiteral("ri")),&race,1);
  auto entry = pets.value(QString::number(race)).toObject();
  if (result.slotMaxLevel <= 0) integer(entry.value(QStringLiteral("stargodSlotMaxLevel")),&result.slotMaxLevel,1);
  QJsonValue jobs = entry.value(QStringLiteral("jobs"));
  if (jobs.isUndefined() && integer(pet.value(QStringLiteral("_metaRaceId")),&alternate,1) && alternate == race)
    jobs = pet.value(QStringLiteral("_metaJobs"));
  if (jobs.isArray()) result.jobs = jobs.toArray();
  else if (jobs.isString()) for (const auto& part : jobs.toString().split(QRegularExpression(QStringLiteral("[#,-]")),Qt::SkipEmptyParts)) {
    int job = 0; if (!integer(part,&job)) { result.jobs = {}; break; } result.jobs.append(job);
  }
  return result;
}

PetBattlePowerState calculatePetBattlePower(const QJsonObject& pet, const PetPowerMetadata& m) {
  PetBattlePowerState out;
  auto reason = [&](const QString& value) { if (!out.unknownReasons.contains(value)) out.unknownReasons.append(value); };
  out.hasServerCurrent = integer(pet.value(QStringLiteral("zdl")),&out.serverCurrent);
  // The observation has its own flag; unavailable local values never inherit it.
  out.hasCurrent = false;
  out.hasExtreme = integer(pet.value(QStringLiteral("xzdl")),&out.extreme);
  const auto current = pet.value(QStringLiteral("czdlv")).toObject(),maximum = pet.value(QStringLiteral("mzdlv")).toObject();
  bool dimensions = !current.isEmpty() && current.keys() == maximum.keys();
  // Complete official replies explicitly supply zero for inactive systems.
  // Matching sparse objects must not masquerade as a fully analysed pet.
  for (const auto* name : {"lv","iv","pl","sgv","ep","gsv","lav","lsv","bsv","asv","sjv"})
    dimensions = dimensions && current.contains(QLatin1String(name));
  int currentBase = 0,maximumBase = 0;
  for (auto i = current.begin(); i != current.end(); ++i) {
    int a = 0,b = 0;
    if (!integer(i.value(),&a) || !integer(maximum.value(i.key()),&b) || !add(out.componentCurrentTotal,a) ||
        !add(out.componentExtremeTotal,b)) { dimensions = false; continue; }
    if (i.key() != QStringLiteral("sgv") && i.key() != QStringLiteral("asv"))
      if (!add(currentBase,a) || !add(maximumBase,b)) dimensions = false;
  }
  out.componentTotalsKnown = dimensions;
  if (!dimensions) reason(QStringLiteral("当前与极限战力分项缺失、维度不一致或数值无效"));
  astrolabe(pet,m,out);
  if (!out.astrolabeApplicabilityKnown) reason(QStringLiteral("缺少星轮序列，无法确认本精灵是否适用星轮突破"));
  else if (!out.astrolabePowerKnown) reason(QStringLiteral("星轮节点定义或状态不完整；点击检查数据更新后重算"));
  if (out.astrolabeApplicable && !out.breakthroughKnown) reason(QStringLiteral("星轮突破状态尚未提供"));
  bool maxLevelKnown = false;
  if (pet.contains(QStringLiteral("stargodSlotMaxLevel"))) maxLevelKnown = integer(pet.value(QStringLiteral("stargodSlotMaxLevel")),&out.stargodMaxLevel,1);
  else if (m.slotMaxLevel > 0) { out.stargodMaxLevel = m.slotMaxLevel; maxLevelKnown = true; }
  QList<Slot> parsedSlots,ordinary,movable;
  out.stargodSlotsKnown = parseSlots(pet.value(QStringLiteral("sgs")),m,&parsedSlots);
  if (!out.stargodSlotsKnown) { parsedSlots.clear(); reason(QStringLiteral("星神槽位序列缺失或格式无效")); }
  bool equippedKnown = out.stargodSlotsKnown,ownedKnown = out.stargodSlotsKnown,catalogKnown = !m.stargods.isEmpty();
  QSet<int> ownedIds,packIds; Groups ownedGroups,allGroups; QList<int> ownedMovable,allMovable; QMap<int,int> ownedQualities;
  out.stargodLevelsFull = out.stargodSlotsKnown && (parsedSlots.isEmpty() || maxLevelKnown);
  for (const auto& slot : parsedSlots) {
    if (maxLevelKnown && slot.level > out.stargodMaxLevel) { maxLevelKnown = false; out.stargodLevelsFull = false; }
    if (slot.level < out.stargodMaxLevel) { ++out.stargodLevelMissingSlots; out.stargodLevelsFull = false; }
    if (slot.changeable) movable.append(slot); else ordinary.append(slot);
    if (slot.id > 0) {
      ownedIds.insert(slot.changeable && slot.source > 0 ? slot.source : slot.id);
      int value = 0;
      if (!power(m,slot.id,slot.level,&value) || !add(out.equippedStargodPower,value)) equippedKnown = false;
      if (!slot.changeable) ++out.equippedStars;
    }
  }
  out.stargodSlots = int(ordinary.size()); out.hasChangeableSlot = !movable.isEmpty();
  if (movable.size() > 1) ownedKnown = false;
  const auto backpack = pet.value(QStringLiteral("sgsp"));
  out.stargodBackpackKnown = backpack.isArray() && backpack.toArray().size() <= 8192;
  if (out.stargodBackpackKnown) for (const auto& raw : backpack.toArray()) {
    int id = 0; if (!integer(raw,&id,1)) { out.stargodBackpackKnown = false; break; }
    packIds.insert(id); ownedIds.insert(id);
    if (!m.stargod(id).value(QStringLiteral("changeable")).toBool()) ++out.backpackStars;
  }
  if (!out.stargodBackpackKnown) { ownedKnown = false; reason(QStringLiteral("缺少本精灵星神背包，不能确认已有红星或真实购买缺口")); }
  for (int id : ownedIds) {
    Star star; if (!starInfo(m,id,&star)) { ownedKnown = false; continue; }
    if (!star.allowed) continue;
    if (star.movable) { ownedMovable.append(id); out.changeableOwnedQuality = qMax(out.changeableOwnedQuality,star.quality); }
    else { ownedGroups[star.type].append(id); ownedQualities[star.type] = qMax(ownedQualities.value(star.type),star.quality); }
  }
  for (auto i = m.stargods.begin(); i != m.stargods.end(); ++i) {
    int id = 0; Star star;
    if (!integer(i.key(),&id,1) || !starInfo(m,id,&star)) { catalogKnown = false; continue; }
    if (!star.allowed) continue;
    if (star.movable) allMovable.append(id); else allGroups[star.type].append(id);
  }
  out.stargodQualitiesKnown = ownedKnown; out.availableStars = int(ownedGroups.size());
  out.missingStars = qMax(0,out.stargodSlots-out.availableStars);
  for (int quality : ownedQualities) { if (quality == 6) ++out.redStars; else if (quality == 5) ++out.goldStars; }
  out.missingRedStars = qMax(0,out.stargodSlots-out.redStars);
  out.stargodFull = ownedKnown && out.missingStars == 0; out.redStargodFull = ownedKnown && out.missingRedStars == 0;
  out.changeableOwnedKnown = ownedKnown; out.changeableOwnedRed = ownedKnown && out.changeableOwnedQuality == 6;
  if (out.hasChangeableSlot && movable.first().id > 0) {
    Star star; const int id = movable.first().source > 0 ? movable.first().source : movable.first().id;
    if (starInfo(m,id,&star)) { out.changeableQuality = star.quality; out.changeableRed = star.quality == 6; }
  }
  const auto best = assign(ordinary,ownedGroups,m);
  const auto trained = maxLevelKnown ? assign(ordinary,ownedGroups,m,out.stargodMaxLevel) : Assignment{};
  const auto supreme = maxLevelKnown ? assign(ordinary,allGroups,m,out.stargodMaxLevel) : Assignment{};
  out.bestOrdinaryStargodPower = best.total;
  out.currentStargodPower = best.total; out.ownedMaxStargodPower = trained.total; out.highestStargodPower = supreme.total;
  ownedKnown = ownedKnown && best.known;
  bool trainedKnown = ownedKnown && maxLevelKnown && trained.known;
  bool supremeKnown = out.stargodSlotsKnown && maxLevelKnown && catalogKnown && supreme.known && !supreme.ids.contains(0);
  QList<PetStargodSlotPower> rows;
  auto rowFor = [&](const Slot& slot,int bestId,int now,int trainedPower,int maxPower,bool nowKnown,bool trainKnown,bool maxKnown) {
    PetStargodSlotPower row; row.slot = slot.index+1; row.level = slot.level; row.changeable = slot.changeable;
    row.equippedId = qMax(0,slot.id); row.ownedBestId = bestId;
    row.equippedKnown = row.equippedId == 0 || power(m,row.equippedId,slot.level,&row.equippedPower);
    row.ownedPower = now; row.ownedMaxPower = trainedPower; row.highestPower = maxPower;
    row.ownedKnown = nowKnown; row.ownedMaxKnown = trainKnown; row.highestKnown = maxKnown;
    row.equippedName = m.stargod(row.equippedId).value(QStringLiteral("name")).toString();
    row.ownedBestName = m.stargod(bestId).value(QStringLiteral("name")).toString(); row.fromBackpack = bestId > 0 && packIds.contains(bestId);
    if (!nowKnown) row.action = QStringLiteral("数据不足");
    else if (bestId != row.equippedId && now > row.equippedPower) row.action = QStringLiteral("已有星神可调整装备");
    else row.action = QStringLiteral("当前装备已满足已有配置");
    if (trainKnown && trainedPower > now) row.action += QStringLiteral("；槽位待升级");
    if (maxKnown && trainKnown && maxPower > trainedPower) row.action += QStringLiteral("；仍需获得星神");
    rows.append(row);
  };
  for (int i = 0; i < ordinary.size(); ++i) rowFor(ordinary[i],best.ids.value(i),best.values.value(i),trained.values.value(i),
      supreme.values.value(i),ownedKnown,trainedKnown,supremeKnown);
  for (const auto& slot : movable) {
    const auto b = changeableAssignment(slot,ownedMovable,m);
    const auto t = maxLevelKnown ? changeableAssignment(slot,ownedMovable,m,out.stargodMaxLevel) : Assignment{};
    const auto s = maxLevelKnown ? changeableAssignment(slot,allMovable,m,out.stargodMaxLevel) : Assignment{};
    out.changeableStargodPower = b.total;
    if (!add(out.currentStargodPower,b.total) || !b.known) ownedKnown = false;
    if (!add(out.ownedMaxStargodPower,t.total) || !t.known) trainedKnown = false;
    if (!add(out.highestStargodPower,s.total) || !s.known || s.ids.value(0) == 0) supremeKnown = false;
    rowFor(slot,b.ids.value(0),b.total,t.total,s.total,ownedKnown,trainedKnown,supremeKnown);
  }
  if (parsedSlots.isEmpty() && out.stargodSlotsKnown) {
    ownedKnown = out.stargodBackpackKnown && ownedIds.isEmpty(); trainedKnown = supremeKnown = ownedKnown;
  }
  std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b) { return a.slot < b.slot; }); out.stargodDetails = rows;
  out.stargodPowerKnown = ownedKnown && equippedKnown; out.ownedMaxStargodPowerKnown = trainedKnown;
  out.stargodAcquisitionKnown = trainedKnown && supremeKnown; out.targetStargodPower = out.currentStargodPower;
  out.adjustmentGain = out.stargodPowerKnown ? qMax(0,out.currentStargodPower-out.equippedStargodPower) : 0;
  out.stargodUpgradeGap = trainedKnown && out.stargodPowerKnown ? qMax(0,out.ownedMaxStargodPower-out.currentStargodPower) : 0;
  out.stargodAcquisitionGap = out.stargodAcquisitionKnown ? qMax(0,out.highestStargodPower-out.ownedMaxStargodPower) : 0;
  if (!ownedKnown) reason(QStringLiteral("星神品质、同类限制或职业适用数据不足，无法确认已有最优配置"));
  if (!maxLevelKnown && !parsedSlots.isEmpty()) reason(QStringLiteral("缺少该精灵的星神槽位等级上限"));
  if (!supremeKnown) reason(QStringLiteral("完整星神目标配置尚不能确认；不以固定八红星代替实际栏位"));
  int rawAstro = 0; const bool rawAstroKnown = integer(current.value(QStringLiteral("asv")),&rawAstro);
  const int usedAstro = out.astrolabePowerKnown ? out.astrolabeCurrent : rawAstro;
  const bool currentAstroKnown = out.astrolabePowerKnown || rawAstroKnown;
  if (out.astrolabePowerKnown && rawAstroKnown && rawAstro != usedAstro)
    reason(QStringLiteral("星轮节点重算与回包分项不同，采用节点配置计算；可能需要刷新该精灵详情"));
  bool currentKnown = dimensions && currentAstroKnown && out.stargodPowerKnown && (parsedSlots.isEmpty() || current.contains(QStringLiteral("sgv")));
  int local = currentBase; currentKnown = currentKnown && add(local,usedAstro) && add(local,out.currentStargodPower);
  if (currentKnown) { out.current = local; out.currentLocallyCalculated = true; out.hasCurrent = true; }
  int equipped = currentBase;
  out.equippedCurrentKnown = dimensions && currentAstroKnown && equippedKnown && add(equipped,usedAstro) && add(equipped,out.equippedStargodPower);
  if (out.equippedCurrentKnown) out.equippedCurrent = equipped;
  int top = maximumBase;
  out.hasHighest = dimensions && supremeKnown && out.astrolabeApplicabilityKnown && out.astrolabePowerKnown &&
      (!out.astrolabeApplicable || out.breakthroughApplicabilityKnown) &&
      add(top,out.astrolabeMaximum) && add(top,out.astrolabeTargetBonus) && add(top,out.highestStargodPower);
  if (out.hasHighest) out.highest = top;
  out.highestGapKnown = out.hasHighest && currentKnown; out.highestGap = out.highestGapKnown ? qMax(0,out.highest-out.current) : 0;
  const QList<QPair<QString,QString>> names = {{QStringLiteral("lv"),QStringLiteral("等级与基础成长")},
    {QStringLiteral("iv"),QStringLiteral("天赋与星能")},{QStringLiteral("pl"),QStringLiteral("职业熟练度")},
    {QStringLiteral("sgv"),QStringLiteral("星神")},{QStringLiteral("ep"),QStringLiteral("精灵装备")},
    {QStringLiteral("gsv"),QStringLiteral("守护石")},{QStringLiteral("lav"),QStringLiteral("学习培养")},
    {QStringLiteral("lsv"),QStringLiteral("传说石")},{QStringLiteral("bsv"),QStringLiteral("元魂")},
    {QStringLiteral("asv"),QStringLiteral("天迹星轮")},{QStringLiteral("sjv"),QStringLiteral("神源兽")}};
  QStringList keys; QMap<QString,QString> labels;
  for (const auto& pair : names) { keys.append(pair.first); labels.insert(pair.first,pair.second); }
  for (const auto& key : current.keys()+maximum.keys()) if (!keys.contains(key)) keys.append(key);
  bool allAtTarget = true;
  for (const auto& key : keys) {
    PetBattlePowerComponent row; row.key = key; row.label = labels.value(key,key); row.explanation = explanation(key);
    row.currentKnown = integer(current.value(key),&row.current); row.extremeKnown = integer(maximum.value(key),&row.extreme);
    row.highest = row.extreme; row.highestKnown = row.extremeKnown; row.applicable = current.contains(key) || maximum.contains(key);
    if (key == QStringLiteral("sgv")) {
      row.applicable = row.applicable || !parsedSlots.isEmpty(); row.current = out.currentStargodPower; row.currentKnown = out.stargodPowerKnown;
      row.highest = out.highestStargodPower; row.highestKnown = supremeKnown;
      row.explanation = QStringLiteral("已装备与本精灵背包合并，按同类去重并排除职业不适用星神，在实际槽等级求最优。官方极限以金星为基准；至高使用全部可用星神的最高合法配置。调整装备、升级槽位与获得缺星分别统计。");
    } else if (key == QStringLiteral("asv")) {
      row.applicable = out.astrolabeApplicable || row.applicable; row.current = usedAstro; row.currentKnown = currentAstroKnown;
      row.highest = out.astrolabeMaximum; row.highestKnown = out.astrolabePowerKnown && add(row.highest,out.astrolabeTargetBonus);
    }
    if (!row.applicable) {
      row.currentKnown = row.extremeKnown = row.highestKnown = false;
      row.explanation = QStringLiteral("本次详情未提供此分项；不纳入总和，不据此判断满培养。");
    }
    row.gapKnown = row.applicable && row.currentKnown && row.highestKnown;
    if (row.gapKnown) row.gap = qMax(0,row.highest-row.current);
    if (row.applicable && (!row.gapKnown || row.gap > 0)) allAtTarget = false;
    if (row.applicable && row.currentKnown && row.extremeKnown && row.current < row.extreme) {
      const int gap = row.extreme-row.current; out.componentGaps.append({key,row.label,row.current,row.extreme,gap});
      if (!add(out.knownExtremeGap,gap)) { out.hasHighest = false; out.highestGapKnown = false; }
    }
    out.components.append(row);
  }
  out.completionKnown = out.highestGapKnown && out.stargodAcquisitionKnown && (!out.astrolabeApplicable ||
      (out.breakthroughApplicabilityKnown && (!out.breakthroughApplicable || out.breakthroughKnown)));
  out.isHighest = out.completionKnown && allAtTarget && out.stargodLevelsFull &&
      (!out.astrolabeApplicable || out.astrolabeTargetBonus == 0 || (out.breakthrough && out.astrolabeSelectedNodes >= 3)) && out.current >= out.highest;
  return out;
}
