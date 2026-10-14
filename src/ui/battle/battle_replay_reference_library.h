#pragma once
#include "ui/battle/battle_replay_buff_presentation.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>

// Reference data describes what the official client can look up. It never
// supplies missing equipment, activation, or causal facts to the battle state.
namespace ReplayReference {
inline QString text(const QJsonValue& value) {
  if (value.isString()) return value.toString();
  if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
  if (value.isBool()) return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
  if (value.isNull() || value.isUndefined()) return {};
  return QString::fromUtf8(value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                                         : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}
inline QString plain(const QString& value) {
  if (!value.contains(QLatin1Char('<')) && !value.contains(QLatin1Char('&'))) return value;
  QTextDocument doc; doc.setHtml(value); return doc.toPlainText();
}
inline QString readable(const QJsonValue& value) {
  if (value.isArray()) {
    QStringList items;
    for (const auto& item : value.toArray()) items << readable(item);
    return items.join(QStringLiteral("；"));
  }
  if (value.isObject()) {
    QStringList items;
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) items << it.key() + QStringLiteral("：") + readable(it.value());
    return items.join(QStringLiteral("；"));
  }
  return plain(text(value));
}
inline QString safe(const QJsonValue& value) {
  return readable(value).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}
inline const QList<QPair<QString, QString>>& categories() {
  static const QList<QPair<QString, QString>> values = {
    {"summon", QStringLiteral("召唤配对")}, {"carry", QStringLiteral("携带／神使／吞噬")},
    {"almighty", QStringLiteral("全能")}, {"hero", QStringLiteral("英雄布阵条件")},
    {"psychics", QStringLiteral("通灵布阵条件")}, {"formationTeams", QStringLiteral("阵容组合")},
    {"stargods", QStringLiteral("星神")}, {"badges", QStringLiteral("元魂")}, {"astrolabe", QStringLiteral("天迹星轮")},
    {"titles", QStringLiteral("精灵称号")}, {"powerFull", QStringLiteral("满气势专属表现")},
    {"resources", QStringLiteral("模型／变身／资源")},
    {"skills", QStringLiteral("战斗技能")}, {"buffs", QStringLiteral("战斗 Buff")},
    {"pets", QStringLiteral("精灵与形态")}, {"officialEntries", QStringLiteral("官方词条")},
    {"transformSkills", QStringLiteral("元能／赋能／幻元")}, {"legends", QStringLiteral("旧传说技能")},
    {"huanMechanics", QStringLiteral("寰力量")}, {"qiYun", QStringLiteral("气运方案")}, {"suLing", QStringLiteral("溯灵方案")},
    {"petDragons", QStringLiteral("界龙")}, {"attributes", QStringLiteral("属性")},
    {"execution", QStringLiteral("执行结果")}, {"bufferExecution", QStringLiteral("队伍执行")},
    {"combos", QStringLiteral("组合播放规则")}, {"coverage", QStringLiteral("引用核对与待补资料")}
  };
  return values;
}
inline QString categoryLabel(const QString& key) {
  for (const auto& item : categories()) if (item.first == key) return item.second;
  return key;
}
inline QString coverageMeaning(const QString& status) {
  if (status == QStringLiteral("official-text-empty")) return QStringLiteral("官方文字空白：条目已存在；名称或说明为空，不等于官方没有解析。播放配置另行核对。");
  if (status == QStringLiteral("missing-reference")) return QStringLiteral("引用资料待核：当前资料表没有匹配目标；尚不能断言官方不解析，也不能按别的表中同号条目补齐。");
  if (status == QStringLiteral("reference-unclassified")) return QStringLiteral("读取关系待核：已知原字段，但尚未确认适用的官方读取分支；候选对应不作为已证实关联。");
  if (status == QStringLiteral("identity-unresolved")) return QStringLiteral("原名对应待核：精灵记录可以读取；展示形态与实际原名的精确对应仍未确认。本局明确记录的本体关系单独使用。");
  if (status == QStringLiteral("term-unmatched")) return QStringLiteral("文字词条待核：保留原文；未匹配到解释词条，不代表缺少一种战斗机制或漏掉一条战报。");
  return QStringLiteral("核对状态尚未分类；保留原始依据。");
}
inline QString availability(const QString& category, const QJsonObject& row) {
  if (category == QStringLiteral("buffs")) {
    QString html = QStringLiteral("<p><b>%1</b></p>").arg(replayBuffAvailability(row).toHtmlEscaped());
    const auto presentation = replayBuffPresentation(row);
    if (!presentation.explanations.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(presentation.explanations.join(QLatin1Char('\n')).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!row[QStringLiteral("bhs")].toString().isEmpty()) html += QStringLiteral("<p>官方伤害标注：%1</p>").arg(safe(row[QStringLiteral("bhs")]));
    return html;
  }
  if (category != QStringLiteral("skills")) return {};
  QStringList parts;
  if (row.isEmpty()) parts << QStringLiteral("本资料库未收录该技能条目；保留战报编号与已记录结果。");
  else {
    parts << (row[QStringLiteral("playbackAvailable")].toBool() ? QStringLiteral("官方播放配置已收录。") :
      row.contains(QStringLiteral("playbackAvailable")) ? QStringLiteral("已收录技能文字，未取得独立播放配置；不据文字补造攻击动画。") : QStringLiteral("已有技能条目，播放配置的完整性尚未核对。"));
    if (row[QStringLiteral("name")].toString().trimmed().isEmpty()) parts << QStringLiteral("官方名称为空。");
    if (row[QStringLiteral("description")].toString().trimmed().isEmpty()) parts << QStringLiteral("官方独立说明为空。");
  }
  return QStringLiteral("<p><b>%1</b></p>").arg(parts.join(QStringLiteral(" ")).toHtmlEscaped());
}
inline QString consumption(const QJsonObject& row) {
  const auto records = row[QStringLiteral("referenceConsumption")].toArray();
  if (records.isEmpty()) return {};
  QString html = QStringLiteral("<h4>官方如何读取这些引用</h4><p>以下是已核对的读取条件与查表结果，不代表本场已触发，也不替代缺少的定义。</p>");
  for (const auto& item : records) {
    const auto record = item.toObject();
    html += QStringLiteral("<p><b>%1 [%2]</b><br>%3</p>").arg(safe(record[QStringLiteral("label")]), safe(record[QStringLiteral("id")]), safe(record[QStringLiteral("summary")]));
    for (const auto& branch : record[QStringLiteral("branches")].toArray()) {
      const auto detail = branch.toObject();
      if (!detail[QStringLiteral("summary")].toString().isEmpty())
        html += QStringLiteral("<p style='color:#66758a'>%1</p>").arg(safe(detail[QStringLiteral("summary")]));
    }
  }
  return html;
}
inline QJsonObject consumptionSources(const QJsonObject& catalog, const QJsonObject& row) {
  auto readings = row[QStringLiteral("referenceConsumption")].toArray();
  for (const auto& value : row[QStringLiteral("references")].toArray())
    for (const auto& reading : value.toObject()[QStringLiteral("referenceConsumption")].toArray()) readings.append(reading);
  const auto sources = catalog[QStringLiteral("referenceConsumers")].toObject()[QStringLiteral("evidence")].toObject();
  QJsonObject result;
  for (const auto& reading : readings) {
    auto ids = reading.toObject()[QStringLiteral("evidenceIds")].toArray();
    for (const auto& branch : reading.toObject()[QStringLiteral("branches")].toArray())
      for (const auto& id : branch.toObject()[QStringLiteral("evidenceIds")].toArray()) ids.append(id);
    for (const auto& id : ids) if (sources.contains(id.toString())) result.insert(id.toString(), sources[id.toString()]);
  }
  return result;
}
inline QJsonObject arrayRows(const QJsonArray& rows) {
  QJsonObject result;
  for (int i = 0; i < rows.size(); ++i) result.insert(QString::number(i), rows[i]);
  return result;
}
inline QJsonObject rows(const QJsonObject& catalog, const QString& category) {
  const auto supplement = catalog[QStringLiteral("replaySupplements")].toObject();
  if (category == QStringLiteral("resources")) {
    QJsonObject result;
    const auto groups = supplement[category].toObject();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
      const auto group = it.value().toObject();
      const auto list = group[QStringLiteral("rows")].toArray();
      // Keep empty official tables and loading rules inspectable as well.
      auto metadata = group; metadata.remove(QStringLiteral("rows"));
      metadata.insert(QStringLiteral("name"), it.key() + QStringLiteral(" · 查询规则"));
      result.insert(it.key() + QStringLiteral("/config"), metadata);
      for (int i = 0; i < list.size(); ++i) result.insert(it.key() + QLatin1Char('/') + QString::number(i), list[i]);
    }
    return result;
  }
  if (supplement.contains(category)) {
    auto result = arrayRows(supplement[category].toObject()[QStringLiteral("rows")].toArray());
    if (category == QStringLiteral("titles")) {
      auto metadata = supplement[category].toObject(); metadata.remove(QStringLiteral("rows"));
      metadata.insert(QStringLiteral("name"), QStringLiteral("精灵称号的记录与读取规则"));
      result.insert(QStringLiteral("config"), metadata);
    }
    return result;
  }
  if (catalog[QStringLiteral("replayCultivation")].toObject().contains(category))
    return catalog[QStringLiteral("replayCultivation")].toObject()[category].toObject()[QStringLiteral("definitions")].toObject();
  if (category == QStringLiteral("legends")) return catalog[QStringLiteral("legendMechanics")].toObject()[QStringLiteral("definitions")].toObject();
  if (category == QStringLiteral("petDragons")) return catalog[category].toObject()[QStringLiteral("definitions")].toObject();
  if (category == QStringLiteral("qiYun") || category == QStringLiteral("suLing"))
    return catalog[QStringLiteral("mechanismRules")].toObject()[category].toObject()[QStringLiteral("plans")].toObject();
  if (category == QStringLiteral("coverage")) return arrayRows(catalog[QStringLiteral("referenceCoverage")].toObject()[QStringLiteral("items")].toArray());
  if (catalog[category].isArray()) return arrayRows(catalog[category].toArray());
  return catalog[category].toObject();
}
inline QString name(const QString& category, const QString& id, const QJsonObject& row) {
  if (category == QStringLiteral("pets")) {
    const auto identity = row[QStringLiteral("identity")].toObject();
    const auto canonical = identity[QStringLiteral("canonicalName")].toString();
    const auto status = identity[QStringLiteral("status")].toString();
    if ((status == QStringLiteral("native") || status == QStringLiteral("skin")) && !canonical.isEmpty()) return canonical;
    return QStringLiteral("原名待核实 [%1]").arg(id);
  }
  for (const auto& field : {"name", "title", "nm", "label", "heroSkillName", "summonSkillName"}) {
    const auto value = plain(row[QLatin1String(field)].toString());
    if (!value.isEmpty()) return value;
  }
  return categoryLabel(category) + QStringLiteral(" [%1]").arg(id);
}
inline QString description(const QJsonObject& row) {
  QStringList values;
  for (const auto& field : {"description", "descX", "desc", "mess", "beffect", "tip", "descriptionTemplate", "autoEffectDescription", "reasonLabel"}) {
    if (QLatin1String(field) == QLatin1String("desc") && !row[QStringLiteral("descX")].toString().isEmpty()) continue;
    const auto value = readable(row[QLatin1String(field)]);
    if (!value.isEmpty() && !values.contains(value)) values << value;
  }
  // Supplements retain named source fields separately. Their display text is
  // already joined in description; do not repeat it with developer field names.
  if (values.isEmpty()) {
    const auto descriptions = row[QStringLiteral("descriptions")];
    if (descriptions.isObject()) {
      const auto fields = descriptions.toObject();
      for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto value = readable(it.value()); if (!value.isEmpty() && !values.contains(value)) values << value;
      }
    } else {
      const auto value = readable(descriptions); if (!value.isEmpty()) values << value;
    }
  }
  return values.join(QLatin1Char('\n'));
}
inline QString searchable(const QString& category, const QString& id, const QJsonObject& row) {
  QString result = id + QLatin1Char(' ') + name(category, id, row) + QLatin1Char(' ') +
    description(row) + QLatin1Char(' ') + readable(row[QStringLiteral("readings")]);
  for (const auto& field : {"id", "name", "defineId", "titleId", "raceId", "skillId", "buffId", "sourceRaceIds", "targetRaceIds", "references"})
    result += QLatin1Char(' ') + readable(row[QLatin1String(field)]);
  for (const auto& field : {"levels", "variants", "channels"}) {
    const auto value = row[QLatin1String(field)];
    if (value.isObject()) {
      const auto values = value.toObject();
      for (auto it = values.begin(); it != values.end(); ++it) result += QLatin1Char(' ') + searchable(category, it.key(), it.value().toObject());
    } else for (const auto& child : value.toArray()) result += QLatin1Char(' ') + searchable(category, {}, child.toObject());
  }
  return result;
}
inline QString link(const QString& category, const QString& id, const QString& label, bool raw = false) {
  const auto payload = QString::fromLatin1(QUrl::toPercentEncoding(category + QLatin1Char('/') + id));
  return QStringLiteral("<a href='%1:%2'>%3</a>").arg(raw ? QStringLiteral("reference-raw") : QStringLiteral("reference"), payload, label.toHtmlEscaped());
}
inline QString indexLink(const QString& category, const QString& label = {}) {
  return QStringLiteral("<a href='reference-list:%1'>%2</a>").arg(category.toHtmlEscaped(), (label.isEmpty() ? categoryLabel(category) : label).toHtmlEscaped());
}
inline QString readingsHtml(const QJsonArray& readings, const QSet<QString>& omitted = {}) {
  QString html;
  for (const auto& item : readings) {
    const auto row = item.toObject();
    if (omitted.contains(row[QStringLiteral("label")].toString())) continue;
    html += QStringLiteral("<p><b>%1</b>：%2</p>").arg(safe(row[QStringLiteral("label")]), safe(row[QStringLiteral("value")]));
  }
  return html;
}
inline QString detail(const QJsonObject& catalog, const QString& category, const QString& id, bool raw = false) {
  const auto definitions = rows(catalog, category);
  const auto row = definitions[id].toObject();
  QString html = QStringLiteral("<h3>%1</h3><p>%2 · %3</p>").arg(name(category, id, row).toHtmlEscaped(), categoryLabel(category).toHtmlEscaped(), id.toHtmlEscaped());
  if (!definitions.contains(id)) return html + QStringLiteral("<p>当前官方资料没有这条记录，保留查询编号。</p>");
  html += QStringLiteral("<p style='color:#66758a'>以下为官方配置资料；本场是否装备、激活或触发，以战报明确记录为准。</p>");
  html += availability(category, row);
  const bool hasLevels = !row[QStringLiteral("levels")].toObject().isEmpty() || !row[QStringLiteral("levels")].toArray().isEmpty();
  if (!hasLevels) html += QStringLiteral("<p>%1</p>").arg(description(row).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  html += readingsHtml(row[QStringLiteral("readings")].toArray(), {QStringLiteral("名称"), QStringLiteral("定义编号"), QStringLiteral("官方检索拼音")});
  if (category == QStringLiteral("pets")) {
    html += QStringLiteral("<p>原表显示名称：%1<br>原名核对状态：%2</p>").arg(safe(row[QStringLiteral("name")]),
      row[QStringLiteral("identity")].toObject()[QStringLiteral("status")].toString() == QStringLiteral("unresolved")
        ? QStringLiteral("缺少精确对应，未把原表形态名认作已核原名") : QStringLiteral("已有官方原生或换肤对应"));
    html += consumption(catalog[QStringLiteral("petMechanics")].toObject()[id].toObject());
  }
  if (category == QStringLiteral("coverage")) {
    html += QStringLiteral("<p><b>%1</b></p>").arg(coverageMeaning(row[QStringLiteral("status")].toString()).toHtmlEscaped());
    const auto itemCategory = row[QStringLiteral("category")].toString();
    const auto target = itemCategory == QStringLiteral("buff-text") ? QStringLiteral("buffs") :
      itemCategory == QStringLiteral("skill-text") || itemCategory == QStringLiteral("pet-skill-reference") ? QStringLiteral("skills") :
      itemCategory == QStringLiteral("pet-identity") ? QStringLiteral("pets") : QString();
    const auto targetId = text(row[QStringLiteral("id")]);
    if (!target.isEmpty() && catalog[target].toObject().contains(targetId)) {
      html += availability(target, catalog[target].toObject()[targetId].toObject());
      html += QStringLiteral("<p>%1</p>").arg(link(target, targetId, QStringLiteral("查看已有官方条目")));
    }
    html += QStringLiteral("<h4>关联记录</h4>");
    for (const auto& item : row[QStringLiteral("references")].toArray()) {
      const auto reference = item.toObject();
      auto referenceCategory = reference[QStringLiteral("namespace")].toString();
      if (referenceCategory == QStringLiteral("petMechanics")) referenceCategory = QStringLiteral("pets");
      const auto referenceId = text(reference[QStringLiteral("id")]);
      const auto label = reference[QStringLiteral("name")].toString();
      const auto caption = (label.isEmpty() ? categoryLabel(referenceCategory) : label) + QStringLiteral(" [%1]").arg(referenceId);
      html += QStringLiteral("<p>%1</p>").arg(catalog[referenceCategory].toObject().contains(referenceId)
        ? link(referenceCategory, referenceId, caption) : caption.toHtmlEscaped());
      const auto relevant = reference[QStringLiteral("referenceConsumption")].toArray();
      if (!relevant.isEmpty()) html += consumption(QJsonObject{{QStringLiteral("referenceConsumption"), relevant}});
    }
  }
  for (const auto& field : {"levels", "variants", "channels"}) {
    auto values = row[QLatin1String(field)].toArray();
    if (row[QLatin1String(field)].isObject()) {
      const auto keyed = row[QLatin1String(field)].toObject();
      for (auto it = keyed.begin(); it != keyed.end(); ++it) values.append(it.value());
    }
    if (QString::fromLatin1(field) == QStringLiteral("levels") && !values.isEmpty()) html += QStringLiteral("<h4>全部 %1 个等级</h4><table width='100%' cellpadding='5' cellspacing='0'>").arg(values.size());
    for (int i = 0; i < values.size(); ++i) {
      const auto value = values[i].toObject();
      const auto level = text(value[QStringLiteral("level")]);
      if (QString::fromLatin1(field) == QStringLiteral("levels")) {
        QStringList metadata;
        for (const auto& reading : value[QStringLiteral("readings")].toArray()) {
          const auto entry = reading.toObject(); const auto label = entry[QStringLiteral("label")].toString();
          if (label == QStringLiteral("等级") || label == QStringLiteral("名称") || label == QStringLiteral("效果说明")) continue;
          metadata << safe(entry[QStringLiteral("label")]) + QStringLiteral("：") + safe(entry[QStringLiteral("value")]);
        }
        html += QStringLiteral("<tr bgcolor='%1'><td width='60' valign='top'><b>等级 %2</b></td><td><b>%3</b><br><span style='color:#66758a'>%4</span></td></tr>")
          .arg(i % 2 ? QStringLiteral("#ffffff") : QStringLiteral("#f2f6fc"), level.toHtmlEscaped(), safe(description(value)), metadata.join(QStringLiteral(" · ")));
        continue;
      }
      html += QStringLiteral("<h4>%1%2</h4>").arg(level.isEmpty() ? QStringLiteral("条件／档位 %1").arg(i + 1) : QStringLiteral("等级 %1").arg(level.toHtmlEscaped()),
        value[QStringLiteral("name")].toString().isEmpty() ? QString() : QStringLiteral(" · ") + safe(value[QStringLiteral("name")]));
      html += readingsHtml(value[QStringLiteral("readings")].toArray());
      html += QStringLiteral("<p>%1</p>").arg(description(value).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
      for (const auto& key : {"label", "thresholds", "effects", "typeToTakeEffect", "valueToTakeEffect", "triggerParam"})
        if (value.contains(QLatin1String(key))) html += QStringLiteral("<p>%1：%2</p>").arg(QString::fromLatin1(key).toHtmlEscaped(), safe(value[QLatin1String(key)]));
    }
    if (QString::fromLatin1(field) == QStringLiteral("levels") && !values.isEmpty()) html += QStringLiteral("</table>");
  }
  if (category == QStringLiteral("stargods") || category == QStringLiteral("badges") || category == QStringLiteral("astrolabe"))
    html += QStringLiteral("<p><b>本场养成快照未记录时，不认定装备和等级；养成编号不与同号战斗 Buff 相连。</b></p>");
  if (category == QStringLiteral("powerFull")) html += QStringLiteral("<p>官方使用这条 Buff 配置播放满气势表现，不据此往本场状态列表添加 Buff。</p>");
  if (category == QStringLiteral("carry") || category == QStringLiteral("summon") || category == QStringLiteral("almighty"))
    html += QStringLiteral("<p>配对表说明可用配置；不能据此认定本场实际携带者或召唤来源。</p>");
  if (row.contains(QStringLiteral("runtimeEligible")) && !row[QStringLiteral("runtimeEligible")].toBool())
    html += QStringLiteral("<p><b>原表有此行，但未被当前官方读取路径选中。</b></p>");
  for (const auto& field : {"sourceRaceIds", "targetRaceIds", "carrierRaceId", "carriedPetRaceId", "raceId", "psychicsCalledPetRaceId", "skillId"}) {
    const auto value = row[QLatin1String(field)];
    if (value.isUndefined() || value.isNull()) continue;
    const auto target = QString::fromLatin1(field) == QStringLiteral("skillId") ? QStringLiteral("skills") : QStringLiteral("pets");
    const auto references = value.isArray() ? value.toArray() : QJsonArray{value};
    QStringList links; QMap<QString, QStringList> groupedPets;
    for (const auto& reference : references) {
      const auto identifier = text(reference);
      if (identifier.isEmpty() || identifier == QStringLiteral("0") || identifier == QStringLiteral("-1")) continue;
      const auto definition = catalog[target].toObject()[identifier].toObject();
      if (target == QStringLiteral("pets")) groupedPets[name(target, identifier, definition)] << link(target, identifier, identifier);
      else links << link(target, identifier, name(target, identifier, definition) + QStringLiteral(" [%1]").arg(identifier));
    }
    for (auto it = groupedPets.begin(); it != groupedPets.end(); ++it)
      links << it.key().toHtmlEscaped() + QStringLiteral("（%1）").arg(it.value().join(QStringLiteral("、")));
    const QMap<QString, QString> labels{{"sourceRaceIds", QStringLiteral("配置来源精灵")}, {"targetRaceIds", QStringLiteral("配置对象精灵")},
      {"carrierRaceId", QStringLiteral("配置携带者")}, {"carriedPetRaceId", QStringLiteral("配置携带对象")},
      {"raceId", QStringLiteral("对应精灵／外观")}, {"psychicsCalledPetRaceId", QStringLiteral("通灵精灵")}, {"skillId", QStringLiteral("技能资料")}};
    if (!links.isEmpty()) html += QStringLiteral("<p><b>%1</b><br>%2</p>").arg(labels.value(QString::fromLatin1(field)), links.join(QStringLiteral(" · ")));
  }
  if (category == QStringLiteral("powerFull")) {
    const auto buffId = text(row[QStringLiteral("buffId")]);
    html += QStringLiteral("<p>%1</p>").arg(link(QStringLiteral("buffs"), buffId, QStringLiteral("查看满气势表现 Buff [%1]").arg(buffId)));
  }
  html += QStringLiteral("<p>%1 · %2</p>").arg(indexLink(category, QStringLiteral("返回分类")),
    link(category, id, raw ? QStringLiteral("收起原始字段") : QStringLiteral("查看全部原始字段与来源"), !raw));
  if (raw) {
    auto record = row;
    if (category == QStringLiteral("pets")) record[QStringLiteral("referenceConsumption")] = catalog[QStringLiteral("petMechanics")].toObject()[id].toObject()[QStringLiteral("referenceConsumption")];
    const auto sources = consumptionSources(catalog, record);
    if (!sources.isEmpty()) record[QStringLiteral("consumerSources")] = sources;
    html += QStringLiteral("<pre>%1</pre>").arg(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Indented)).toHtmlEscaped());
  }
  return html;
}
inline QStringList sortedIds(const QJsonObject& rows) {
  auto keys = rows.keys();
  std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
    bool an = false, bn = false; const auto av = a.toLongLong(&an), bv = b.toLongLong(&bn);
    if (an != bn) return an;
    return an && bn && av != bv ? av < bv : a < b;
  });
  return keys;
}
inline QString context(const QJsonObject& catalog, const QString& race, const QString& skill = {}) {
  const auto supplements = catalog[QStringLiteral("replaySupplements")].toObject();
  QStringList links;
  for (const auto& group : {"summon", "carry", "almighty", "hero", "psychics"}) {
    const auto key = QString::fromLatin1(group); const auto data = supplements[key].toObject();
    const bool skillGroup = key == QStringLiteral("hero") || key == QStringLiteral("psychics");
    if (skillGroup && skill.isEmpty()) continue;
    if (!skillGroup && race.isEmpty()) continue;
    QJsonValue ids;
    if (skillGroup) ids = data[QStringLiteral("bySkillId")].toObject()[skill];
    else if (key == QStringLiteral("summon") || key == QStringLiteral("carry")) ids = data[QStringLiteral("bySourceRaceId")].toObject()[race];
    else ids = data[QStringLiteral("byRaceId")].toObject()[race];
    auto list = ids.isArray() ? ids.toArray() : ids.isUndefined() || ids.isNull() ? QJsonArray{} : QJsonArray{ids};
    const auto records = data[QStringLiteral("rows")].toArray();
    if (key == QStringLiteral("summon") || key == QStringLiteral("carry")) {
      for (int i = 0; i < records.size(); ++i) {
        for (const auto& target : records[i].toObject()[QStringLiteral("targetRaceIds")].toArray()) {
          if (text(target) == race) { if (!list.contains(QJsonValue(i))) list.append(i); break; }
        }
      }
    }
    for (const auto& id : list) {
      const int index = id.toInt(-1); if (index < 0 || index >= records.size()) continue;
      const auto indexText = QString::number(index);
      links << link(key, indexText, categoryLabel(key) + QStringLiteral(" · ") + name(key, indexText, records[index].toObject()));
    }
  }
  if (!race.isEmpty()) {
    const auto combinations = supplements[QStringLiteral("formationTeams")].toObject()[QStringLiteral("rows")].toArray();
    for (int i = 0; i < combinations.size(); ++i) {
      const auto row = combinations[i].toObject(); bool match = false;
      for (const auto& alternatives : row[QStringLiteral("petAlternatives")].toArray())
        for (const auto& option : alternatives.toArray()) if (text(option) == race) match = true;
      if (match) links << link(QStringLiteral("formationTeams"), QString::number(i), QStringLiteral("阵容组合 · ") + name(QStringLiteral("formationTeams"), QString::number(i), row));
    }
  }
  if (links.isEmpty()) return {};
  links.removeDuplicates();
  return QStringLiteral("<h4>关联官方资料</h4><p>按官方查询字段连接；配对与布阵条件不作为本场生效证明。</p><p>%1</p>").arg(links.join(QStringLiteral("<br>")));
}
inline QJsonObject powerFull(const QJsonObject& catalog, const QString& faceId) {
  const auto group = catalog[QStringLiteral("replaySupplements")].toObject()[QStringLiteral("powerFull")].toObject();
  if (group.isEmpty()) return {};
  const auto value = group[QStringLiteral("byRaceId")].toObject()[faceId];
  const auto records = group[QStringLiteral("rows")].toArray();
  const int index = value.toInt(-1);
  if (index >= 0 && index < records.size()) return records[index].toObject();
  // This is the documented getPowerFullBuffId fallback when the exact face
  // has neither an override nor a dictionary item. Never substitute raceId.
  const auto overrides = group[QStringLiteral("raceIdOverrides")].toObject();
  return QJsonObject{{"buffId", overrides.value(faceId).toString(group[QStringLiteral("defaultBuffId")].toString())},
                    {"branch", overrides.contains(faceId) ? QStringLiteral("explicit-race-map") : QStringLiteral("default-without-dictionary")}};
}
inline QString recordedTitles(const QJsonObject& catalog, const QJsonValue& recorded) {
  if (!recorded.isArray() && !recorded.isObject()) return {};
  const auto array = recorded.isArray() ? recorded.toArray() : QJsonArray{recorded};
  QJsonObject byId;
  for (const auto& value : array) {
    const auto entry = value.toObject(); const auto id = text(entry[QStringLiteral("titleId")]);
    if (!id.isEmpty()) byId.insert(id, entry); // Official PetTitleData: later ID wins.
  }
  if (byId.isEmpty()) return {};
  const auto titles = catalog[QStringLiteral("replaySupplements")].toObject()[QStringLiteral("titles")].toObject();
  QString html = QStringLiteral("<h4>本场称号记录</h4>");
  for (auto it = byId.begin(); it != byId.end(); ++it) {
    const auto entry = it.value().toObject(); const auto index = titles[QStringLiteral("byId")].toObject()[it.key()];
    html += QStringLiteral("<p><b>称号 [%1]</b><br>记录内容：%2<br>扩展记录：%3<br>%4</p>").arg(it.key().toHtmlEscaped(),
      safe(entry[QStringLiteral("content")]), safe(entry[QStringLiteral("extId")]),
      link(QStringLiteral("titles"), index.isUndefined() ? QStringLiteral("config") : text(index), QStringLiteral("查看对应官方读取与资源资料")));
  }
  return html + QStringLiteral("<p>已查主服务及声明的独立表现资源，可读资料里仍未取得称号名称说明；不借用同编号玩家头衔。</p>");
}
} // namespace ReplayReference
