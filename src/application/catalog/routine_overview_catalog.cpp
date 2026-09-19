#include "routine_overview_catalog.h"
#include "protocol/packet_contract.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <climits>

namespace {

QStringList splitArguments(const QString& text) {
  QStringList result;
  QString current;
  bool quoted = false;
  bool escaped = false;
  for (QChar character : text) {
    if (escaped) {
      current.append(character);
      escaped = false;
    } else if (character == QLatin1Char('\\') && quoted) {
      current.append(character);
      escaped = true;
    } else if (character == QLatin1Char('"')) {
      quoted = !quoted;
      current.append(character);
    } else if (character == QLatin1Char(',') && !quoted) {
      result.append(current.trimmed());
      current.clear();
    } else {
      current.append(character);
    }
  }
  result.append(current.trimmed());
  return result;
}

QString decodedQuoted(QString value) {
  value = value.trimmed();
  if (value.size() < 2 || !value.startsWith(QLatin1Char('"')) ||
      !value.endsWith(QLatin1Char('"')))
    return value;
  QJsonParseError error{};
  const QJsonDocument document =
      QJsonDocument::fromJson((QStringLiteral("[") + value + QStringLiteral("]")).toUtf8(), &error);
  return error.error == QJsonParseError::NoError && document.isArray()
             ? document.array().at(0).toString()
             : value.mid(1, value.size() - 2);
}

int asNumber(QString value) {
  value = value.trimmed();
  if (value.contains(QStringLiteral("MAX_VALUE"))) {
    QString compact;
    compact.reserve(value.size());
    for (QChar character : value) if (!character.isSpace()) compact.append(character);
    return compact == QStringLiteral("Number.MAX_VALUE") ? INT_MAX : -1;
  }
  bool valid = false;
  const int parsed = value.toInt(&valid);
  return valid && parsed >= 0 && QString::number(parsed) == value ? parsed : -1;
}

QVector<int> parseNumberArray(const QString& text, const QString& constant) {
  const QRegularExpression expression(
      QStringLiteral("%1\\s*[^=]*=\\s*\\[([^\\]]*)\\]")
          .arg(QRegularExpression::escape(constant)));
  const QRegularExpressionMatch match = expression.match(text);
  QVector<int> result;
  if (!match.hasMatch()) return result;
  for (const QString& value : match.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts))
    result.append(asNumber(value));
  return result;
}

bool extractJsonObject(const QString& text, const QString& marker, QJsonObject* object) {
  const int markerIndex = text.indexOf(marker);
  if (markerIndex < 0) return false;
  const int begin = text.indexOf(QLatin1Char('{'), markerIndex < 0 ? 0 : markerIndex);
  if (begin < 0) return false;
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  int end = -1;
  for (int index = begin; index < text.size(); ++index) {
    const QChar character = text.at(index);
    if (escaped) {
      escaped = false;
      continue;
    }
    if (character == QLatin1Char('\\') && quoted) {
      escaped = true;
      continue;
    }
    if (character == QLatin1Char('"')) {
      quoted = !quoted;
      continue;
    }
    if (quoted) continue;
    if (character == QLatin1Char('{')) ++depth;
    if (character == QLatin1Char('}') && --depth == 0) {
      end = index;
      break;
    }
  }
  if (end <= begin) return false;
  QJsonParseError error{};
  const QJsonDocument document =
      QJsonDocument::fromJson(text.mid(begin, end - begin + 1).toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  *object = document.object();
  return true;
}

bool collectHudEntries(const QJsonValue& value, QHash<QString, QJsonObject>* entries, int depth = 0) {
  if (depth > 64 || entries->size() > 10000) return false;
  if (value.isArray()) {
    for (const QJsonValue& child : value.toArray()) if (!collectHudEntries(child, entries, depth + 1)) return false;
    return true;
  }
  if (!value.isObject()) return true;
  const QJsonObject object = value.toObject();
  const QString key = object.value(QStringLiteral("key")).toString().trimmed();
  const QString name = object.value(QStringLiteral("name")).toString().trimmed();
  const QString service = object.value(QStringLiteral("tryGetService")).toString();
  if (!key.isEmpty() && !name.isEmpty() && !service.isEmpty() &&
      (service.contains(QStringLiteral("NewActivityService")) ||
       object.contains(QStringLiteral("startTime")) ||
       object.contains(QStringLiteral("redPointId"))))
    entries->insert(key, object);
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
    if (!collectHudEntries(iterator.value(), entries, depth + 1)) return false;
  return true;
}

void collectRedDescendants(int id, const QHash<int, QVector<int>>& graph,
                           QSet<int>* visited) {
  QList<int> pending{id};
  while (!pending.isEmpty()) {
    const int current = pending.takeLast();
    if (current <= 0 || visited->contains(current)) continue;
    visited->insert(current);
    for (int child : graph.value(current)) if (!visited->contains(child)) pending.append(child);
  }
}

QJsonObject builtInRoot() {
  const QList<RoutineTaskDefinition> defaults = {
      {1, QStringLiteral("猎取星神"), 1, 15, 15, 3, 15},
      {2, QStringLiteral("农场收获"), 1, 15, 6, 2, 32},
      {4, QStringLiteral("采摘缤纷树"), 1, 15, 13, INT_MAX, 15},
      {6, QStringLiteral("竞技场"), 1, 20, 32, 8, 15},
      {7, QStringLiteral("源兽之门"), 1, 15, 13, INT_MAX, 15},
      {8, QStringLiteral("在线奖励"), 0, 0, 5, INT_MAX, 15},
      {12, QStringLiteral("参与灵骑破封"), 1, 5, 90, 30, 4},
      {19, QStringLiteral("领取红钻"), 1, 15, 0, 0, 0},
      {21, QStringLiteral("联盟签到"), 1, 10, 7, 1, 15},
      {22, QStringLiteral("参与排位赛"), 1, 15, 15, INT_MAX, 15},
      {24, QStringLiteral("每日签到"), 1, 15, 7, 1, 15},
      {30, QStringLiteral("参与星轮探险"), 1, 15, 10, INT_MAX, 15},
      {38, QStringLiteral("VIP 周签到"), 0, 0, 1, 1, 15},
      {39, QStringLiteral("抵御入侵领奖"), 0, 0, 8, INT_MAX, 15}};
  QJsonArray tasks;
  for (const RoutineTaskDefinition& task : defaults)
    tasks.append(QJsonObject{{QStringLiteral("id"), task.id},
                             {QStringLiteral("name"), task.name},
                             {QStringLiteral("dayFinish"), task.dayFinish},
                             {QStringLiteral("dayActive"), task.dayActive},
                             {QStringLiteral("weekFinish"), task.weekFinish},
                             {QStringLiteral("weekDailyMax"), task.weekDailyMax},
                             {QStringLiteral("weekActive"), task.weekActive}});
  return QJsonObject{{QStringLiteral("schema"), 1},
                     {QStringLiteral("tasks"), tasks},
                     {QStringLiteral("dayPrizeThresholds"), QJsonArray{10, 20, 30, 60, 100}},
                     {QStringLiteral("weekPrizeThresholds"), QJsonArray{150, 300, 600, 900, 1200}},
                     {QStringLiteral("activities"), QJsonArray{}}};
}

}  // namespace

RoutineOverviewCatalog& RoutineOverviewCatalog::instance() {
  static RoutineOverviewCatalog catalog;
  return catalog;
}

RoutineOverviewCatalog::RoutineOverviewCatalog() {
  snapshot_ = prepare(builtInRoot(), QStringLiteral("程序内置日常/周常配置"), {}, nullptr);
  if (!snapshot_) snapshot_ = std::make_shared<RoutineCatalogSnapshot>();
}

std::shared_ptr<const RoutineCatalogSnapshot> RoutineOverviewCatalog::prepare(const QJsonObject& root, const QString& source,
                                        const QDateTime& updatedAt, QString* error) {
  const auto invalid = [error]() -> std::shared_ptr<const RoutineCatalogSnapshot> {
    if (error) *error = QStringLiteral("任务/活动目录结构或数字无效，保留上一份完整目录");
    return {};
  };
  const auto integer = [](const QJsonValue& value, int minimum = 0) {
    return value.isDouble() && PacketContracts::checkedInteger(value, nullptr, minimum, INT_MAX);
  };
  if (!integer(root.value(QStringLiteral("schema")), 1) || root.value(QStringLiteral("schema")).toInt() != 1)
    return invalid();
  for (const QString& array : {QStringLiteral("tasks"), QStringLiteral("activities"),
           QStringLiteral("dayPrizeThresholds"), QStringLiteral("weekPrizeThresholds")})
    if (!root.value(array).isArray()) return invalid();
  QSet<int> taskIds;
  for (const auto& value : root.value(QStringLiteral("tasks")).toArray()) {
    if (!value.isObject()) return invalid();
    const auto task = value.toObject();
    if (!integer(task.value(QStringLiteral("id")), 1) ||
        !task.value(QStringLiteral("name")).isString() || task.value(QStringLiteral("name")).toString().trimmed().isEmpty())
      return invalid();
    const int id = task.value(QStringLiteral("id")).toInt();
    if (taskIds.contains(id)) return invalid();
    taskIds.insert(id);
    for (const QString& field : {QStringLiteral("dayFinish"), QStringLiteral("dayActive"),
             QStringLiteral("weekFinish"), QStringLiteral("weekDailyMax"), QStringLiteral("weekActive")})
      if (!integer(task.value(field))) return invalid();
  }
  QSet<QString> activityKeys;
  for (const auto& value : root.value(QStringLiteral("activities")).toArray()) {
    if (!value.isObject()) return invalid();
    const auto activity = value.toObject();
    for (const QString& field : {QStringLiteral("key"), QStringLiteral("name")})
      if (!activity.value(field).isString() || activity.value(field).toString().trimmed().isEmpty()) return invalid();
    const auto key = activity.value(QStringLiteral("key")).toString();
    if (activityKeys.contains(key)) return invalid();
    activityKeys.insert(key);
    if (activity.contains(QStringLiteral("redPointId")) && !integer(activity.value(QStringLiteral("redPointId")))) return invalid();
    if (activity.contains(QStringLiteral("redPointIds"))) {
      if (!activity.value(QStringLiteral("redPointIds")).isArray()) return invalid();
      for (const auto& id : activity.value(QStringLiteral("redPointIds")).toArray()) if (!integer(id, 1)) return invalid();
    }
    const auto start = activity.value(QStringLiteral("startTime"));
    if (!start.isUndefined() && (!start.isString() || (!start.toString().isEmpty() &&
        !QDate::fromString(start.toString(), QStringLiteral("yyyyMMdd")).isValid()))) return invalid();
  }
  for (const QString& field : {QStringLiteral("dayPrizeThresholds"), QStringLiteral("weekPrizeThresholds")}) {
    int previous = 0;
    for (const auto& value : root.value(field).toArray()) {
      if (!integer(value, 1) || value.toInt() <= previous) return invalid();
      previous = value.toInt();
    }
  }
  QList<RoutineTaskDefinition> tasks;
  for (const QJsonValue& value : root.value(QStringLiteral("tasks")).toArray()) {
    const QJsonObject object = value.toObject();
    RoutineTaskDefinition task;
    task.id = object.value(QStringLiteral("id")).toInt();
    task.name = object.value(QStringLiteral("name")).toString();
    task.dayFinish = object.value(QStringLiteral("dayFinish")).toInt();
    task.dayActive = object.value(QStringLiteral("dayActive")).toInt();
    task.weekFinish = object.value(QStringLiteral("weekFinish")).toInt();
    task.weekDailyMax = object.value(QStringLiteral("weekDailyMax")).toInt();
    task.weekActive = object.value(QStringLiteral("weekActive")).toInt();
    if (task.id > 0 && !task.name.isEmpty()) tasks.append(task);
  }
  if (tasks.isEmpty()) {
    if (error) *error = QStringLiteral("没有解析到日常/周常任务");
    return {};
  }
  QList<ActivityOverviewDefinition> activities;
  for (const QJsonValue& value : root.value(QStringLiteral("activities")).toArray()) {
    const QJsonObject object = value.toObject();
    ActivityOverviewDefinition activity;
    activity.key = object.value(QStringLiteral("key")).toString();
    activity.name = object.value(QStringLiteral("name")).toString();
    activity.startDate = QDate::fromString(object.value(QStringLiteral("startTime")).toString(),
                                           QStringLiteral("yyyyMMdd"));
    activity.redPointId = object.value(QStringLiteral("redPointId")).toInt();
    for (const QJsonValue& id : object.value(QStringLiteral("redPointIds")).toArray())
      activity.redPointIds.append(id.toInt());
    if (!activity.key.isEmpty() && !activity.name.isEmpty()) activities.append(activity);
  }
  QVector<int> day;
  for (const QJsonValue& value : root.value(QStringLiteral("dayPrizeThresholds")).toArray())
    day.append(value.toInt());
  QVector<int> week;
  for (const QJsonValue& value : root.value(QStringLiteral("weekPrizeThresholds")).toArray())
    week.append(value.toInt());
  static std::atomic<quint64> revisions{0};
  auto next = std::make_shared<RoutineCatalogSnapshot>();
  next->revision = ++revisions;
  next->root = root;
  next->tasks = tasks;
  next->activities = activities;
  next->dayPrizeThresholds = day;
  next->weekPrizeThresholds = week;
  next->sourceLabel = source;
  next->sourceUpdatedAt = updatedAt;
  return next;
}

QJsonObject RoutineOverviewCatalog::parseOfficialTexts(QString taskText, const QString& hudText,
    const QString& redText, QString* error) {
  taskText.replace(QStringLiteral("Number\n      .MAX_VALUE"), QStringLiteral("Number.MAX_VALUE"));
  taskText.replace(QStringLiteral("Number\r\n      .MAX_VALUE"), QStringLiteral("Number.MAX_VALUE"));

  QJsonArray tasks;
  const QRegularExpression taskExpression(
      QStringLiteral("new\\s+DiamondTaskDefine\\s*\\(([^\\)]*)\\)"),
      QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatchIterator taskMatches = taskExpression.globalMatch(taskText);
  while (taskMatches.hasNext()) {
    if (tasks.size() >= 10000) {
      if (error) *error = QStringLiteral("官方任务数量超过目录预算");
      return {};
    }
    const QStringList fields = splitArguments(taskMatches.next().captured(1));
    if (fields.size() < 7) {
      if (error) *error = QStringLiteral("官方日常任务参数不完整，保留旧目录");
      return {};
    }
    tasks.append(QJsonObject{{QStringLiteral("id"), asNumber(fields.at(0))},
                             {QStringLiteral("name"), decodedQuoted(fields.at(1))},
                             {QStringLiteral("dayFinish"), asNumber(fields.at(2))},
                             {QStringLiteral("dayActive"), asNumber(fields.at(3))},
                             {QStringLiteral("weekFinish"), asNumber(fields.at(4))},
                             {QStringLiteral("weekDailyMax"), asNumber(fields.at(5))},
                             {QStringLiteral("weekActive"), asNumber(fields.at(6))}});
  }
  if (tasks.isEmpty()) {
    if (error) *error = QStringLiteral("官方日常任务格式无法识别，已保留旧配置");
    return {};
  }

  QJsonObject hudRoot;
  if (!extractJsonObject(hudText, QStringLiteral("DATA:Object"), &hudRoot)) {
    if (error) *error = QStringLiteral("官方 HUD 活动配置格式无法识别，已保留旧配置");
    return {};
  }
  QHash<QString, QJsonObject> hudEntries;
  if (!collectHudEntries(hudRoot.value(QStringLiteral("hud")), &hudEntries)) {
    if (error) *error = QStringLiteral("官方活动目录深度或数量超过预算");
    return {};
  }

  QHash<int, QVector<int>> redGraph;
  const QRegularExpression redExpression(
      QStringLiteral("new\\s+RedPointConfigNode\\s*\\(\\s*(\\d+)\\s*(?:,\\s*\\[([^\\]]*)\\])?\\s*\\)"));
  QRegularExpressionMatchIterator redMatches = redExpression.globalMatch(redText);
  int redReferences = 0;
  while (redMatches.hasNext()) {
    if (++redReferences > 50000) {
      if (error) *error = QStringLiteral("官方红点关系数量超过目录预算");
      return {};
    }
    const QRegularExpressionMatch match = redMatches.next();
    const int id = asNumber(match.captured(1));
    if (id <= 0 || redGraph.contains(id)) {
      if (error) *error = QStringLiteral("官方红点目录标识无效或重复");
      return {};
    }
    QVector<int> children;
    QSet<int> seenChildren;
    if (!match.captured(2).trimmed().isEmpty()) {
      for (const QString& value : match.captured(2).split(QLatin1Char(','), Qt::KeepEmptyParts)) {
        const int child = asNumber(value);
        if (child <= 0 || ++redReferences > 50000) {
          if (error) *error = QStringLiteral("官方红点目录子标识无效");
          return {};
        }
        if (!seenChildren.contains(child)) { seenChildren.insert(child); children.append(child); }
      }
    }
    redGraph.insert(id, children);
  }

  QList<QString> keys = hudEntries.keys();
  std::sort(keys.begin(), keys.end(), [&hudEntries](const QString& left, const QString& right) {
    const QString leftDate = hudEntries.value(left).value(QStringLiteral("startTime")).toString();
    const QString rightDate = hudEntries.value(right).value(QStringLiteral("startTime")).toString();
    if (leftDate != rightDate) return leftDate > rightDate;
    return left < right;
  });
  QJsonArray activities;
  for (const QString& key : keys) {
    const QJsonObject entry = hudEntries.value(key);
    qint64 redPoint = 0;
    if (entry.contains(QStringLiteral("redPointId")) &&
        !PacketContracts::checkedInteger(entry.value(QStringLiteral("redPointId")), &redPoint, 0, INT_MAX)) {
      if (error) *error = QStringLiteral("官方活动目录红点标识无效");
      return {};
    }
    const int redPointId = static_cast<int>(redPoint);
    QSet<int> redIds;
    collectRedDescendants(redPointId, redGraph, &redIds);
    QList<int> sortedIds = redIds.values();
    std::sort(sortedIds.begin(), sortedIds.end());
    QJsonArray ids;
    for (int id : sortedIds) ids.append(id);
    activities.append(QJsonObject{{QStringLiteral("key"), key},
                                  {QStringLiteral("name"), entry.value(QStringLiteral("name"))},
                                  {QStringLiteral("startTime"), entry.value(QStringLiteral("startTime"))},
                                  {QStringLiteral("redPointId"), redPointId},
                                  {QStringLiteral("redPointIds"), ids}});
  }

  QJsonArray dayThresholds;
  for (int value : parseNumberArray(taskText, QStringLiteral("DAY_PRIZE_PROGRESS1")))
    dayThresholds.append(value);
  QJsonArray weekThresholds;
  for (int value : parseNumberArray(taskText, QStringLiteral("WEEK_PRIZE_PROGRESS")))
    weekThresholds.append(value);
  const QJsonObject root{{QStringLiteral("schema"), 1},
                         {QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                         {QStringLiteral("tasks"), tasks},
                         {QStringLiteral("dayPrizeThresholds"), dayThresholds},
                         {QStringLiteral("weekPrizeThresholds"), weekThresholds},
                         {QStringLiteral("activities"), activities}};
  return root;
}
