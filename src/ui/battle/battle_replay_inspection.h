#pragma once

#include "domain/battle_replay.h"
#include <QJsonArray>
#include <QJsonDocument>

// Board-facing explanations consume observations only. They never repair a
// missing value or select a historical configuration from the filename.
namespace ReplayInspection {
inline QString esc(const QString& text) { return text.toHtmlEscaped(); }
inline QString json(const QJsonValue& value) {
  if (value.isUndefined()) return QStringLiteral("字段缺失");
  if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson());
  if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson());
  return BattleReplay::valueText(value);
}
inline QJsonValue atPath(QJsonValue value, const QString& path) {
  if (path.isEmpty()) return QJsonValue(QJsonValue::Undefined);
  for (QString token : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    token.replace(QStringLiteral("~1"), QStringLiteral("/")); token.replace(QStringLiteral("~0"), QStringLiteral("~"));
    if (value.isObject()) value = value.toObject()[token];
    else if (value.isArray()) {
      bool ok = false; const int index = token.toInt(&ok); const auto array = value.toArray();
      if (!ok || index < 0 || index >= array.size()) return QJsonValue(QJsonValue::Undefined);
      value = array[index];
    } else return QJsonValue(QJsonValue::Undefined);
  }
  return value;
}
struct UnknownValue { QString label, reason, path; QJsonValue raw; };
inline QList<UnknownValue> unknownValues(const BattleReplay::Fighter& f, const QJsonObject& root) {
  QList<UnknownValue> result;
  const auto append = [&](const QString& label, const QString& path, const QString& fallback) {
    const auto raw = atPath(root, path);
    QString reason = fallback;
    if (!path.isEmpty() && raw.isUndefined()) reason = QStringLiteral("来源字段未记录，不能补成 0");
    else if (raw.isNull()) reason = QStringLiteral("原字段明确为 null，没有可确认的数值");
    else if (raw.isObject() || raw.isArray()) reason += QStringLiteral("；以下保留包含相关变化的原记录");
    else if (!raw.isUndefined() && !raw.isString() && !raw.isDouble()) reason = QStringLiteral("原字段类型不能作为有效数值使用");
    result.append({label, reason, path, raw});
  };
  if (!f.representer) {
    if (!f.hp) append(QStringLiteral("生命"), f.hpPath, QStringLiteral("记录尚不能确定当前生命；查看原值与本步诊断"));
    if (!f.maxHp) append(QStringLiteral("生命上限"), f.maxHpPath.isEmpty() ? f.path + QStringLiteral("/maxHp") : f.maxHpPath, QStringLiteral("生命上限未确认"));
    if (!f.power) append(QStringLiteral("气势"), f.powerPath, QStringLiteral("记录尚不能确定当前气势；查看原值与本步诊断"));
    if (!f.shield) append(QStringLiteral("护盾"), f.shieldPath, QStringLiteral("当前护盾余额无法由已确认的记录确定"));
    if (!f.maxPower) result.append({QStringLiteral("气势上限"), QStringLiteral("当前形态缺少可用的官方满气势配置；不按 100 补齐"), QStringLiteral("官方精灵字典 · 种族 ") + f.raceId, QJsonValue(QJsonValue::Undefined)});
    if (!f.attributesKnown) result.append({QStringLiteral("属性"), QStringLiteral("当前属性尚未被有效记录确认"), f.attributesPath, atPath(root, f.attributesPath)});
    if (!f.entryKnown) result.append({QStringLiteral("登场状态"), QStringLiteral("记录不能确定当前是否已登场"), f.presencePath, atPath(root, f.presencePath)});
  }
  if (!f.buffsKnown) result.append({QStringLiteral("Buff 集合"), QStringLiteral("存在未能确认的初始化或变化，当前集合不完整"), f.buffPath, atPath(root, f.buffPath)});
  return result;
}
inline QString unknownHtml(const BattleReplay::Fighter& f, const QJsonObject& root, const BattleReplay::Step& step) {
  const auto values = unknownValues(f, root);
  if (values.isEmpty()) return {};
  QString html = QStringLiteral("<h4>当前待核实值</h4>");
  for (const auto& value : values)
    html += QStringLiteral("<p><b>%1</b>：%2<br>依据：%3</p><pre>%4</pre>")
      .arg(esc(value.label), esc(value.reason), esc(value.path.isEmpty() ? QStringLiteral("本步未提供可定位字段") : value.path), esc(json(value.raw)));
  for (const auto& note : step.playbackNotes) html += QStringLiteral("<p>%1</p>").arg(esc(note));
  return html;
}
inline QString recordedMetadata(const BattleReplay::Fighter& f, const QJsonObject& catalog) {
  QStringList lines;
  const auto add = [&](const QString& key, const QString& label) {
    if (!f.raw.contains(key)) return;
    lines.append(QStringLiteral("%1：%2").arg(esc(label), esc(json(f.raw[key]))));
  };
  if (f.representer) {
    // PlayerInfo.parseServerResponce: fp/sp/fm/fl. Missing is not zero.
    add(QStringLiteral("id"), QStringLiteral("玩家 ID"));
    add(QStringLiteral("rn"), QStringLiteral("替代显示名"));
    add(QStringLiteral("fp"), QStringLiteral("阵容战力"));
    add(QStringLiteral("sp"), QStringLiteral("阵容速度"));
    add(QStringLiteral("fm"), QStringLiteral("阵型 ID"));
    const auto formationId = BattleReplay::valueText(f.raw[QStringLiteral("fm")]);
    const auto formationName = catalog[QStringLiteral("clientPresentation")].toObject()[QStringLiteral("formations")].toObject()[formationId].toObject()[QStringLiteral("name")].toString();
    if (!formationName.isEmpty()) lines.append(QStringLiteral("官方阵型名：%1").arg(esc(formationName)));
    add(QStringLiteral("fl"), QStringLiteral("阵型等级"));
  } else {
    add(QStringLiteral("lvl"), QStringLiteral("开局等级"));
    add(QStringLiteral("title"), QStringLiteral("称号原始记录"));
    add(QStringLiteral("oRaceId"), QStringLiteral("开局原始种族 ID"));
  }
  return lines.isEmpty() ? QStringLiteral("<p>战报没有提供更多阵容资料。</p>") :
    QStringLiteral("<h4>战报中的阵容资料</h4><p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
}
inline QString configurationHtml(const BattleReplay::Replay& replay, const QJsonObject& catalog) {
  const auto provenance = catalog[QStringLiteral("provenance")].toObject();
  const auto battle = replay.battlePath.isEmpty() ? replay.root : atPath(replay.root, replay.battlePath).toObject();
  QString html = QStringLiteral("<h3>本场配置与播放依据</h3><p>原始数值与判定来自战报；紫色客户端提示使用下列官方配置。历史资源版本尚未逐场核对。</p>");
  html += QStringLiteral("<p>数据版本 ver：%1<br>播放规则：已核对的 Flash 执行链<br>配置取得时间：%2</p>")
    .arg(esc(json(battle[QStringLiteral("ver")])), esc(provenance[QStringLiteral("retrievedUtc")].toString()));
  const auto rechecked = provenance[QStringLiteral("versionChainRecheck")].toObject()[QStringLiteral("retrievedUtc")].toString();
  if (!rechecked.isEmpty()) html += QStringLiteral("<p>官方版本链最近核对：%1</p>").arg(esc(rechecked));
  html += QStringLiteral("<p>ver 是回包数据版本，不能单独判断当时使用 Flash 还是 H5。当前没有混入 H5 独立动画特例。</p>");
  for (const auto& value : provenance[QStringLiteral("packages")].toArray()) {
    const auto package = value.toObject();
    html += QStringLiteral("<p><b>%1</b><br>版本：%2<br>SHA-256：%3</p>")
      .arg(esc(package[QStringLiteral("resource")].toString()), esc(package[QStringLiteral("version")].toString()), esc(package[QStringLiteral("sha256")].toString()));
  }
  html += QStringLiteral("<p>未收录的末击配置、Buff 名称或施加来源继续标为待核实。条内数字不采用原版动画的随机拆分结果；动画时长用于阅读，不表示原版特效帧数。</p>");
  html += QStringLiteral("<p>本场原文 SHA-256：%1</p>").arg(esc(replay.sha256));
  return html;
}
} // namespace ReplayInspection
