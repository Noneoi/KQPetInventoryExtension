#include "battle_replay_catalog_validation.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace {
bool strings(const QJsonObject& row, std::initializer_list<const char*> keys) {
  for (const auto* key : keys) if (!row.value(QLatin1String(key)).isString()) return false;
  return true;
}
bool validRow(const QString& table, const QString& id, const QJsonObject& row) {
  if (table == QStringLiteral("pets")) {
    const auto identity = row.value(QStringLiteral("identity")).toObject();
    return strings(row, {"name"}) && strings(identity, {"status", "canonicalName"}) &&
        row.value(QStringLiteral("nativeAttributes")).isArray() && row.value(QStringLiteral("maxPower")).isDouble();
  }
  const auto key = table == QStringLiteral("execution") ? QStringLiteral("code") :
      table == QStringLiteral("bufferExecution") ? QStringLiteral("bufferId") : QStringLiteral("id");
  if (row.value(key) != QJsonValue(id)) return false;
  if (table == QStringLiteral("skills"))
    return strings(row, {"name", "description", "configResolution"}) &&
        row.value(QStringLiteral("textSource")).isObject() && row.value(QStringLiteral("playbackAvailable")).isBool();
  if (table == QStringLiteral("buffs"))
    return strings(row, {"did", "nm", "df", "once", "pos", "beffect", "bhs", "mess", "nameStatus", "configSource"});
  if (table == QStringLiteral("attributes")) return strings(row, {"nm", "pri", "rcs"});
  if (table == QStringLiteral("execution")) return strings(row, {"nm", "eid", "type"});
  return strings(row, {"actionResId", "skillId", "lightedSkillId", "desc"});
}
} // namespace

QString validateBattleReplayCatalog(const QJsonObject& root) {
  if (root.value(QStringLiteral("format")) != QJsonValue(3))
    return QStringLiteral("回放资料格式不兼容，需要本程序支持的 format 3 完整资料");
  // Format 3 is the complete generated catalog. Do not accept the smaller H5
  // description table, or a partial override that silently loses evidence.
  for (const auto* key : {"provenance", "decompiledSourceSha256", "skills", "buffs",
       "skillLastEffects", "buffPowerFullMap", "relationsBySkill", "relationsByBuff",
       "officialEntries", "officialEntryMentions", "entryMentionsByBuff", "entryMentionsBySkill",
       "petMechanics", "mechanismRules", "legendMechanics", "huanMechanics", "petMechanicsSummary",
       "referenceConsumers", "petIdentitySummary", "clientPresentation", "petDragons",
       "replaySupplements", "replayCultivation", "tableJoinSummary", "pets", "transformSkills",
       "attributes", "execution", "bufferExecution", "referenceCoverage"}) {
    if (!root.value(QLatin1String(key)).isObject())
      return QStringLiteral("回放资料缺少完整的 %1 对象").arg(QLatin1String(key));
  }
  for (const auto* key : {"tableRelations", "combos"}) {
    if (!root.value(QLatin1String(key)).isArray())
      return QStringLiteral("回放资料缺少完整的 %1 列表").arg(QLatin1String(key));
  }
  for (const auto* key : {"pets", "skills", "buffs", "attributes", "execution", "bufferExecution"}) {
    const auto rows = root.value(QLatin1String(key)).toObject();
    if (rows.isEmpty()) return QStringLiteral("回放资料的 %1 表为空").arg(QLatin1String(key));
    for (auto it = rows.begin(); it != rows.end(); ++it) {
      // Empty names/descriptions and nonnumeric official keys are legitimate;
      // missing fields, wrong types or a mismatched row ID are not.
      if (it.key().isEmpty() || !it.value().isObject() || !validRow(QLatin1String(key), it.key(), it.value().toObject()))
        return QStringLiteral("回放资料的 %1 表含无效记录").arg(QLatin1String(key));
    }
  }
  const auto summary = root.value(QStringLiteral("tableJoinSummary")).toObject();
  const auto skillSummary = summary.value(QStringLiteral("skills")).toObject();
  const auto buffSummary = summary.value(QStringLiteral("buffs")).toObject();
  if (skillSummary.value(QStringLiteral("skills")) != QJsonValue(root.value(QStringLiteral("skills")).toObject().size()) ||
      buffSummary.value(QStringLiteral("canonicalBuffIds")) != QJsonValue(root.value(QStringLiteral("buffs")).toObject().size()) ||
      buffSummary.value(QStringLiteral("flashBufferExecuteRows")) != QJsonValue(root.value(QStringLiteral("bufferExecution")).toObject().size()) ||
      buffSummary.value(QStringLiteral("flashComboRows")) != QJsonValue(root.value(QStringLiteral("combos")).toArray().size()) ||
      root.value(QStringLiteral("petMechanicsSummary")).toObject().value(QStringLiteral("pets")) !=
          QJsonValue(root.value(QStringLiteral("pets")).toObject().size()))
    return QStringLiteral("回放资料记录数与生成摘要不一致，可能不是完整资料");
  if (root.value(QStringLiteral("clientPresentation")).toObject()
          .value(QStringLiteral("schemaVersion")) != QJsonValue(1))
    return QStringLiteral("回放表现配置版本不兼容");
  const auto provenance = root.value(QStringLiteral("provenance")).toObject();
  static const QRegularExpression sha256(QStringLiteral("^[0-9a-fA-F]{64}$"));
  if (!sha256.match(provenance.value(QStringLiteral("manifestSha256")).toString()).hasMatch() ||
      provenance.value(QStringLiteral("versiondata")).toObject().value(QStringLiteral("version")).toString().isEmpty() ||
      !provenance.value(QStringLiteral("packages")).isArray())
    return QStringLiteral("回放资料缺少来源版本和校验记录");
  QSet<QString> resources;
  for (const auto& value : provenance.value(QStringLiteral("packages")).toArray()) {
    const auto package = value.toObject();
    const auto resource = package.value(QStringLiteral("resource")).toString();
    const QUrl url(package.value(QStringLiteral("url")).toString());
    if (resource.isEmpty() || resources.contains(resource) ||
        package.value(QStringLiteral("version")).toString().isEmpty() ||
        !sha256.match(package.value(QStringLiteral("sha256")).toString()).hasMatch() ||
        !url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host() != QStringLiteral("aoqi.100bt.com") || !url.path().startsWith(QStringLiteral("/play/")))
      return QStringLiteral("回放资料的官方资源来源记录不完整或重复");
    resources.insert(resource);
  }
  for (const auto* key : {"library/interfaces", "battle/battle", "battleconfig/battleconfig"})
    if (!resources.contains(QLatin1String(key)))
      return QStringLiteral("回放资料缺少 %1 的来源记录").arg(QLatin1String(key));
  return {};
}
