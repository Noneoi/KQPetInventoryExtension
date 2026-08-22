#include "routine_overview_catalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <climits>

namespace {

QJsonObject readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object()
             : QJsonObject{};
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
  return file.commit();
}

QString newestFile(const QString& root, const QString& fileName) {
  QString best;
  qulonglong bestVersion = 0;
  QDirIterator iterator(root, {fileName}, QDir::Files, QDirIterator::Subdirectories);
  const QRegularExpression versionExpression(QStringLiteral("~(\\d+)_decomp"));
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    const QRegularExpressionMatch match = versionExpression.match(path);
    const qulonglong version = match.hasMatch() ? match.captured(1).toULongLong() : 0;
    if (best.isEmpty() || version > bestVersion ||
        (version == bestVersion && QFileInfo(path).lastModified() > QFileInfo(best).lastModified())) {
      best = path;
      bestVersion = version;
    }
  }
  return best;
}

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
  value.remove(QRegularExpression(QStringLiteral("\\s+")));
  if (value.contains(QStringLiteral("MAX_VALUE"))) return INT_MAX;
  return value.toInt();
}

QVector<int> parseNumberArray(const QString& text, const QString& constant) {
  const QRegularExpression expression(
      QStringLiteral("%1\\s*[^=]*=\\s*\\[([^\\]]*)\\]")
          .arg(QRegularExpression::escape(constant)));
  const QRegularExpressionMatch match = expression.match(text);
  QVector<int> result;
  if (!match.hasMatch()) return result;
  for (const QString& value : match.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts))
    result.append(value.trimmed().toInt());
  return result;
}

bool extractJsonObject(const QString& text, const QString& marker, QJsonObject* object) {
  const int markerIndex = text.indexOf(marker);
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

void collectHudEntries(const QJsonValue& value, QHash<QString, QJsonObject>* entries) {
  if (value.isArray()) {
    for (const QJsonValue& child : value.toArray()) collectHudEntries(child, entries);
    return;
  }
  if (!value.isObject()) return;
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
    collectHudEntries(iterator.value(), entries);
}

void collectRedDescendants(int id, const QHash<int, QVector<int>>& graph,
                           QSet<int>* visited) {
  if (id <= 0 || visited->contains(id)) return;
  visited->insert(id);
  for (int child : graph.value(id)) collectRedDescendants(child, graph, visited);
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
  loadObject(builtInRoot(), QStringLiteral("程序内置日常/周常配置"), {}, nullptr);
}

bool RoutineOverviewCatalog::loadObject(const QJsonObject& root, const QString& source,
                                        const QDateTime& updatedAt, QString* error) {
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
    return false;
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
  tasks_ = tasks;
  activities_ = activities;
  dayPrizeThresholds_ = day.isEmpty() ? QVector<int>{10, 20, 30, 60, 100} : day;
  weekPrizeThresholds_ = week.isEmpty() ? QVector<int>{150, 300, 600, 900, 1200} : week;
  sourceLabel_ = source;
  sourceUpdatedAt_ = updatedAt;
  return true;
}

bool RoutineOverviewCatalog::reloadFromDataRoot(const QString& dataRoot, QString* error) {
  const QString path = QDir(dataRoot).filePath(QStringLiteral("catalog/routine-overview.json"));
  if (!QFileInfo::exists(path)) return false;
  return loadObject(readObject(path), QStringLiteral("本地官方任务/活动配置缓存"),
                    QFileInfo(path).lastModified(), error);
}

bool RoutineOverviewCatalog::updateFromOfficialData(const QString& dataRoot, QString* error) {
  QString officialRoot = qEnvironmentVariable("KQPET_OFFICIAL_UNPACK_ROOT");
  if (officialRoot.isEmpty()) officialRoot = QStringLiteral("D:/奥奇工程/奥奇传说解包");
  const QString taskPath = newestFile(officialRoot, QStringLiteral("DiamondTaskConfig.as"));
  const QString hudPath = newestFile(officialRoot, QStringLiteral("CommonHudConfig.as"));
  const QString redPath = newestFile(officialRoot, QStringLiteral("RedPointConfig.as"));
  if (taskPath.isEmpty() || hudPath.isEmpty() || redPath.isEmpty()) {
    if (error) *error = QStringLiteral("官方解包缺少 DiamondTaskConfig/CommonHudConfig/RedPointConfig");
    return false;
  }
  QFile taskFile(taskPath), hudFile(hudPath), redFile(redPath);
  if (!taskFile.open(QIODevice::ReadOnly) || !hudFile.open(QIODevice::ReadOnly) ||
      !redFile.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("无法读取官方任务/活动配置");
    return false;
  }
  QString taskText = QString::fromUtf8(taskFile.readAll());
  taskText.replace(QStringLiteral("Number\n      .MAX_VALUE"), QStringLiteral("Number.MAX_VALUE"));
  taskText.replace(QStringLiteral("Number\r\n      .MAX_VALUE"), QStringLiteral("Number.MAX_VALUE"));
  const QString hudText = QString::fromUtf8(hudFile.readAll());
  const QString redText = QString::fromUtf8(redFile.readAll());

  QJsonArray tasks;
  const QRegularExpression taskExpression(
      QStringLiteral("new\\s+DiamondTaskDefine\\s*\\(([^\\)]*)\\)"),
      QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatchIterator taskMatches = taskExpression.globalMatch(taskText);
  while (taskMatches.hasNext()) {
    const QStringList fields = splitArguments(taskMatches.next().captured(1));
    if (fields.size() < 7) continue;
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
    return false;
  }

  QJsonObject hudRoot;
  if (!extractJsonObject(hudText, QStringLiteral("DATA:Object"), &hudRoot)) {
    if (error) *error = QStringLiteral("官方 HUD 活动配置格式无法识别，已保留旧配置");
    return false;
  }
  QHash<QString, QJsonObject> hudEntries;
  collectHudEntries(hudRoot.value(QStringLiteral("hud")), &hudEntries);

  QHash<int, QVector<int>> redGraph;
  const QRegularExpression redExpression(
      QStringLiteral("new\\s+RedPointConfigNode\\s*\\(\\s*(\\d+)\\s*(?:,\\s*\\[([^\\]]*)\\])?\\s*\\)"));
  QRegularExpressionMatchIterator redMatches = redExpression.globalMatch(redText);
  while (redMatches.hasNext()) {
    const QRegularExpressionMatch match = redMatches.next();
    QVector<int> children;
    for (const QString& value : match.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts))
      children.append(value.trimmed().toInt());
    redGraph.insert(match.captured(1).toInt(), children);
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
    const int redPointId = entry.value(QStringLiteral("redPointId")).toInt();
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
                         {QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                         {QStringLiteral("taskSource"), taskPath},
                         {QStringLiteral("hudSource"), hudPath},
                         {QStringLiteral("redPointSource"), redPath},
                         {QStringLiteral("tasks"), tasks},
                         {QStringLiteral("dayPrizeThresholds"), dayThresholds},
                         {QStringLiteral("weekPrizeThresholds"), weekThresholds},
                         {QStringLiteral("activities"), activities}};
  const QDateTime updatedAt = (std::max)({QFileInfo(taskPath).lastModified(),
                                          QFileInfo(hudPath).lastModified(),
                                          QFileInfo(redPath).lastModified()});
  const QList<RoutineTaskDefinition> oldTasks = tasks_;
  const QList<ActivityOverviewDefinition> oldActivities = activities_;
  const QVector<int> oldDay = dayPrizeThresholds_;
  const QVector<int> oldWeek = weekPrizeThresholds_;
  if (!loadObject(root, QStringLiteral("官方解包动态任务/活动配置"), updatedAt, error))
    return false;
  const QString target = QDir(dataRoot).filePath(QStringLiteral("catalog/routine-overview.json"));
  if (!writeObject(target, root)) {
    tasks_ = oldTasks;
    activities_ = oldActivities;
    dayPrizeThresholds_ = oldDay;
    weekPrizeThresholds_ = oldWeek;
    if (error) *error = QStringLiteral("无法原子写入任务/活动配置缓存");
    return false;
  }
  return true;
}
