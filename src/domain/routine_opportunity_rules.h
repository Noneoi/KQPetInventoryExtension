#pragma once

#include "domain/checked_json_numbers.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <limits>
#include <optional>

// Public rules are separate from account observations. Missing legacy rules
// leave server-supplied counters intact; only derived amounts are unavailable.
namespace RoutineOpportunityRules {

struct Rules {
  std::optional<qint64> starDailyDeclared;
  std::optional<qint64> starWeekly;
  std::optional<qint64> competitionDaily;
  std::optional<qint64> competitionWeeklyDeclared;
  QJsonArray farmInviteBonus;
  QHash<QString, qint64> arenaDaily;
};

inline std::optional<qint64> integer(const QJsonValue& value) {
  qint64 result = 0;
  if (!value.isDouble() || !DomainNumeric::checkedInteger(value, &result, 0,
        std::numeric_limits<int>::max())) return {};
  return result;
}

inline bool keys(const QJsonObject& object, const QStringList& expected) {
  auto actual = object.keys(); auto sorted = expected;
  actual.sort(); sorted.sort();
  return actual == sorted;
}

inline bool sha(const QJsonValue& value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return value.isString() && pattern.match(value.toString()).hasMatch();
}

inline QByteArray canonical(const QJsonValue& value) {
  // Python's rules digest sorts object keys; keep this explicit rather than
  // depending on QJsonObject insertion/serialization order.
  if (value.isObject()) {
    const auto object = value.toObject(); auto names = object.keys(); names.sort();
    QByteArray result("{");
    for (const auto& name : names) {
      if (result.size() > 1) result += ',';
      auto encoded = QJsonDocument(QJsonArray{name}).toJson(QJsonDocument::Compact);
      result += encoded.mid(1, encoded.size() - 2) + ':' + canonical(object.value(name));
    }
    return result + '}';
  }
  if (value.isArray()) {
    QByteArray result("[");
    for (const auto& child : value.toArray()) {
      if (result.size() > 1) result += ',';
      result += canonical(child);
    }
    return result + ']';
  }
  auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return encoded.mid(1, encoded.size() - 2);
}

inline Rules read(const QJsonObject& catalog, bool* valid = nullptr) {
  if (valid) *valid = true;
  if (!catalog.contains(QStringLiteral("opportunityRules"))) return {};
  const auto invalid = [valid]() { if (valid) *valid = false; return Rules{}; };
  if (!catalog.value(QStringLiteral("opportunityRules")).isObject()) return invalid();
  const auto root = catalog.value(QStringLiteral("opportunityRules")).toObject();
  if (integer(root.value(QStringLiteral("schema"))) != 1 || !keys(root,
      {QStringLiteral("schema"), QStringLiteral("starAdventure"), QStringLiteral("nationalCompetition"),
       QStringLiteral("farm"), QStringLiteral("arena")})) return invalid();
  const auto resources = catalog.value(QStringLiteral("source")).toObject().value(QStringLiteral("resources")).toObject();
  struct Source { const char* label; const char* field; const char* key; };
  const Source sources[] = {
    {"starRules", "starAdventure", "newactivityext/newact20220602/starwheeladventure/starwheeladventure"},
    {"competitionRules", "nationalCompetition", "newactivityext/newact20240705/nationalcompetition/nationalcompetition"},
    {"farmRules", "farm", "newactivityext/newact20260522/farmv2/farmv2"},
    {"arenaRules", "arena", "arenav3/arenav3service"}};
  static const QRegularExpression versionPattern(QStringLiteral("^[0-9]{8,20}$"));
  for (const auto& binding : sources) {
    const QString label = QString::fromLatin1(binding.label), field = QString::fromLatin1(binding.field);
    const QString key = QString::fromLatin1(binding.key);
    const auto source = resources.value(label).toObject();
    const auto version = source.value(QStringLiteral("version")).toString();
    if (!root.value(field).isObject() || source.value(QStringLiteral("key")).toString() != key ||
        !versionPattern.match(version).hasMatch() || !sha(source.value(QStringLiteral("sha256"))) ||
        source.value(QStringLiteral("url")).toString() != QStringLiteral("https://aoqi.100bt.com/play/") + key + QLatin1Char('~') + version + QStringLiteral(".swf")) return invalid();
    const auto digest = QCryptographicHash::hash(canonical(root.value(field)), QCryptographicHash::Sha256).toHex();
    if (source.value(QStringLiteral("rulesSha256")).toString().toLatin1() != digest) return invalid();
    for (const auto& name : {QStringLiteral("scripts"), QStringLiteral("contracts")}) {
      if (!source.value(name).isObject() || source.value(name).toObject().isEmpty()) return invalid();
      const auto records = source.value(name).toObject();
      for (auto entry = records.begin(); entry != records.end(); ++entry)
        if (entry.key().isEmpty() || !sha(entry.value())) return invalid();
    }
  }
  Rules result;
  const auto star = root.value(QStringLiteral("starAdventure")).toObject();
  const auto competition = root.value(QStringLiteral("nationalCompetition")).toObject();
  if (!keys(star, {QStringLiteral("dailyDeclaredLimit"), QStringLiteral("weeklyLimit")}) ||
      !keys(competition, {QStringLiteral("dailyBase"), QStringLiteral("weeklyDeclaredLimit")})) return invalid();
  result.starDailyDeclared = integer(star.value(QStringLiteral("dailyDeclaredLimit")));
  result.starWeekly = integer(star.value(QStringLiteral("weeklyLimit")));
  result.competitionDaily = integer(competition.value(QStringLiteral("dailyBase")));
  result.competitionWeeklyDeclared = integer(competition.value(QStringLiteral("weeklyDeclaredLimit")));
  if (!result.starDailyDeclared || !result.starWeekly || !result.competitionDaily || !result.competitionWeeklyDeclared) return invalid();
  const auto farm = root.value(QStringLiteral("farm")).toObject();
  if (!keys(farm, {QStringLiteral("inviteBonus")}) || !farm.value(QStringLiteral("inviteBonus")).isArray()) return invalid();
  result.farmInviteBonus = farm.value(QStringLiteral("inviteBonus")).toArray();
  if (result.farmInviteBonus.isEmpty() || result.farmInviteBonus.size() > 100) return invalid();
  for (qsizetype i = 0; i < result.farmInviteBonus.size(); ++i) {
    const auto row = result.farmInviteBonus.at(i).toObject();
    if (!keys(row, {QStringLiteral("count"), QStringLiteral("dailyRefresh"), QStringLiteral("refreshMax"),
                   QStringLiteral("dailyPick"), QStringLiteral("pickMax")})) return invalid();
    for (auto value = row.begin(); value != row.end(); ++value) if (!integer(value.value())) return invalid();
    if (row.value(QStringLiteral("count")).toInt() != i ||
        row.value(QStringLiteral("dailyRefresh")).toInt() > row.value(QStringLiteral("refreshMax")).toInt() ||
        row.value(QStringLiteral("dailyPick")).toInt() > row.value(QStringLiteral("pickMax")).toInt()) return invalid();
  }
  const auto arena = root.value(QStringLiteral("arena")).toObject();
  if (!keys(arena, {QStringLiteral("zao1"), QStringLiteral("zao2")})) return invalid();
  qint64 firstField = 0;
  for (const QString& key : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
    const auto row = arena.value(key).toObject();
    const auto field = integer(row.value(QStringLiteral("fieldId"))), base = integer(row.value(QStringLiteral("dailyBase")));
    if (!keys(row, {QStringLiteral("fieldId"), QStringLiteral("dailyBase")}) || !field || *field <= 0 || !base) return invalid();
    if (key == QStringLiteral("zao1")) firstField = *field;
    else if (*field != firstField + 1) return invalid();
    result.arenaDaily.insert(key, *base);
  }
  return result;
}

inline std::optional<qint64> remaining(const std::optional<qint64>& base,
    const std::optional<qint64>& bought, const std::optional<qint64>& used) {
  qint64 total = 0;
  if (!base || !bought || !used || !DomainNumeric::checkedAdd(*base, *bought, &total) ||
      total > std::numeric_limits<int>::max()) return {};
  // NC_MainPanel.updatePanel / ArenaV3FieldInfo.parseInfo both use Math.max(0,...).
  return qMax<qint64>(0, total - *used);
}

inline std::optional<qint64> farmInviteCount(const QJsonObject& packet) {
  if (packet.contains(QStringLiteral("inviteCount"))) {
    qint64 value = 0;
    if (!DomainNumeric::checkedInteger(packet.value(QStringLiteral("inviteCount")), &value, 0, std::numeric_limits<int>::max())) return {};
    return value;
  }
  // FV2Client.getInfo -> _parseInviteString -> FV2Model.parseData. Only count
  // syntactically understood records; names/user identifiers are never exposed.
  const auto raw = packet.value(QStringLiteral("in"));
  if (!raw.isString() || raw.toString().size() > 64 * 1024) return {};
  qint64 count = 0;
  for (const auto& record : raw.toString().split(QLatin1Char('#'), Qt::KeepEmptyParts)) {
    if (record.isEmpty() || record == QStringLiteral("-1")) continue;
    const auto parts = record.split(QLatin1Char(':'));
    if (parts.size() < 2) continue;
    const auto identity = parts[0].split(QLatin1Char('|'));
    if (identity.size() < 2) continue;
    qint64 id = 0;
    if (!DomainNumeric::checkedInteger(identity[0], &id, 0, std::numeric_limits<int>::max())) return {};
    if (id > 0) ++count;
  }
  return count;
}

inline QJsonObject farmBonus(const Rules& rules, const QJsonObject& packet) {
  const auto invites = farmInviteCount(packet);
  if (!invites || rules.farmInviteBonus.isEmpty()) return {};
  return rules.farmInviteBonus.at(static_cast<qsizetype>(qMin(*invites, qint64(rules.farmInviteBonus.size() - 1)))).toObject();
}

}  // namespace RoutineOpportunityRules
