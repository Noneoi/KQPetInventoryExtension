#include "routine_overview_controller.h"

#include "pet_repository.h"
#include "routine_overview_catalog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace {

const QString kDailyCommand = QStringLiteral("1008_20170623_dt_0");
const QString kRedPointCommand = QStringLiteral("1037_0");
const QString kStarWheelCommand = QStringLiteral("1008_20220603_swa_0_0");
const QString kArenaCommand = QStringLiteral("16_24_A");
const QString kPetParkFusionCommand = QStringLiteral("100_13_0");
const QString kPetParkFeedCommand = QStringLiteral("100_2_0");

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

QJsonObject packetObject(const QString& payload) {
  QJsonParseError error{};
  QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
  if (error.error == QJsonParseError::NoError && document.isObject()) return document.object();
  const int begin = payload.indexOf(QLatin1Char('{'));
  const int end = payload.lastIndexOf(QLatin1Char('}'));
  if (begin < 0 || end <= begin) return {};
  document = QJsonDocument::fromJson(payload.mid(begin, end - begin + 1).toUtf8(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object()
             : QJsonObject{};
}

}  // namespace

RoutineOverviewController::RoutineOverviewController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  timeout_ = new QTimer(this);
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this]() {
    for (const QString& command : std::as_const(pendingCommands_))
      requestWarnings_.append(QStringLiteral("%1超时").arg(requestLabels_.value(command, command)));
    pendingCommands_.clear();
    finish(anyUpdated_,
           QStringLiteral("%1；未返回部分保留旧缓存")
               .arg(requestWarnings_.join(QStringLiteral("、"))));
  });
  if (repository_) {
    account_ = repository_->accountKey();
    sessionGeneration_ = repository_->sessionGeneration();
    connect(repository_, &PetRepository::accountSessionChanged, this,
            &RoutineOverviewController::changeSession);
    RoutineOverviewCatalog::instance().reloadFromDataRoot(repository_->dataRoot());
    loadCache();
  }
}

void RoutineOverviewController::setSender(Sender sender) { sender_ = std::move(sender); }

bool RoutineOverviewController::requestRefresh() {
  if (running_) return false;
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) {
    emit statusChanged(QStringLiteral("请先登录并识别账号，再手动刷新任务与活动状态"));
    return false;
  }
  if (!sender_) {
    emit statusChanged(QStringLiteral("原版发送入口尚未就绪"));
    return false;
  }
  QString catalogError;
  if (RoutineOverviewCatalog::instance().updateFromOfficialData(repository_->dataRoot(),
                                                                 &catalogError))
    emit catalogUpdated();
  else if (!catalogError.isEmpty())
    emit statusChanged(QStringLiteral("活动目录更新失败，继续使用旧配置：%1").arg(catalogError));

  requestAccount_ = account_;
  requestSessionGeneration_ = sessionGeneration_;
  anyUpdated_ = false;
  pendingCommands_.clear();
  requestLabels_.clear();
  requestWarnings_.clear();
  running_ = true;
  emit runningChanged(true);
  emit statusChanged(QStringLiteral("正在手动查询任务、活动红点和玩法剩余次数……"));
  const auto sendRequest = [this](const QString& service, const QString& command,
                                  const QString& parameters, const QString& label) {
    requestLabels_.insert(command, label);
    pendingCommands_.insert(command);
    if (sender_(service, command, parameters)) return;
    pendingCommands_.remove(command);
    requestWarnings_.append(QStringLiteral("%1请求发送失败").arg(label));
  };
  sendRequest(QStringLiteral("TimelinessActExtension"), kDailyCommand,
              QStringLiteral("null"), QStringLiteral("日常/周常"));
  sendRequest(QStringLiteral("null"), kRedPointCommand,
              QStringLiteral("{\"ids\":\"lights\"}"), QStringLiteral("活动红点"));
  sendRequest(QStringLiteral("TimelinessActExtension"), kStarWheelCommand,
              QStringLiteral("null"), QStringLiteral("星轮探险次数"));
  sendRequest(QStringLiteral("null"), kArenaCommand, QStringLiteral("null"),
              QStringLiteral("竞技场次数"));
  sendRequest(QStringLiteral("PetParkExtension"), kPetParkFusionCommand,
              QStringLiteral("null"), QStringLiteral("精灵公园6合1次数"));
  const QString feedParameters = QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("m"), account_}})
          .toJson(QJsonDocument::Compact));
  sendRequest(QStringLiteral("PetParkExtension"), kPetParkFeedCommand,
              feedParameters, QStringLiteral("精灵公园带回次数"));
  if (pendingCommands_.isEmpty()) {
    finish(false, QStringLiteral("发送日常/周常与活动状态查询失败"));
    return false;
  }
  timeout_->start(10000);
  return true;
}

void RoutineOverviewController::handlePacket(const QString& method, const QString& payload) {
  if (method != QStringLiteral("recivedata") || !running_) return;
  const QJsonObject packet = packetObject(payload);
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  if (!pendingCommands_.contains(command)) return;
  if (!repository_ || !repository_->isAuthenticated() || requestAccount_ != account_ ||
      requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration()) {
    finish(false, QStringLiteral("账号已切换，已忽略旧账号的任务/活动响应"));
    return;
  }
  if (packet.contains(QStringLiteral("r")) && packet.value(QStringLiteral("r")).toInt() != 1) {
    completeRequest(command, false,
                    QStringLiteral("%1被服务器拒绝").arg(requestLabels_.value(command)));
    return;
  }
  if (command == kDailyCommand) {
    if (!packet.value(QStringLiteral("ti")).isArray() ||
        !packet.value(QStringLiteral("wti")).isArray()) {
      completeRequest(command, false, QStringLiteral("日常/周常数据不完整"));
      return;
    }
    dailyPacket_ = packet;
    hasDailyPacket_ = true;
    completeRequest(command, true);
  } else if (command == kRedPointCommand) {
    QSet<int> points;
    const QString raw = packet.value(QStringLiteral("rs")).toString();
    for (const QString& value : raw.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
      bool ok = false;
      const int id = value.toInt(&ok);
      if (ok && id > 0) points.insert(id);
    }
    activeRedPoints_ = points;
    hasRedPointPacket_ = true;
    completeRequest(command, true);
  } else {
    bool valid = false;
    if (command == kStarWheelCommand)
      valid = packet.value(QStringLiteral("ti")).isDouble() &&
              packet.value(QStringLiteral("wgt")).isDouble();
    else if (command == kArenaCommand)
      valid = packet.value(QStringLiteral("sweep")).isDouble();
    else if (command == kPetParkFusionCommand)
      valid = packet.value(QStringLiteral("pt")).isDouble();
    else if (command == kPetParkFeedCommand)
      valid = packet.value(QStringLiteral("rfc")).isDouble();
    if (!valid) {
      completeRequest(command, false,
                      QStringLiteral("%1数据不完整").arg(requestLabels_.value(command)));
      return;
    }
    opportunityPackets_.insert(command, packet);
    completeRequest(command, true);
  }
}

void RoutineOverviewController::completeRequest(const QString& command, bool updated,
                                                const QString& warning) {
  if (!pendingCommands_.remove(command)) return;
  if (updated) {
    anyUpdated_ = true;
    saveCache();
  } else if (!warning.isEmpty()) {
    requestWarnings_.append(warning);
  }
  if (pendingCommands_.isEmpty()) {
    const QString status = requestWarnings_.isEmpty()
                               ? QStringLiteral("已更新任务、活动红点与已接入玩法的真实剩余次数")
                               : QStringLiteral("已更新可用数据；%1，该部分保留旧缓存")
                                     .arg(requestWarnings_.join(QStringLiteral("、")));
    finish(anyUpdated_, status);
  } else if (updated) {
    emit dataUpdated();
  }
}

void RoutineOverviewController::finish(bool publish, const QString& status) {
  timeout_->stop();
  pendingCommands_.clear();
  running_ = false;
  emit runningChanged(false);
  emit statusChanged(status);
  if (publish) emit dataUpdated();
}

void RoutineOverviewController::changeSession(const QString& account, quint64 generation) {
  timeout_->stop();
  const bool wasRunning = running_;
  running_ = false;
  requestAccount_.clear();
  requestSessionGeneration_ = 0;
  account_ = account;
  sessionGeneration_ = generation;
  dailyPacket_ = {};
  activeRedPoints_.clear();
  opportunityPackets_ = {};
  hasDailyPacket_ = false;
  hasRedPointPacket_ = false;
  loadCache();
  if (wasRunning) emit runningChanged(false);
  emit dataUpdated();
  emit statusChanged((hasDailyPacket_ || hasRedPointPacket_ || !opportunityPackets_.isEmpty())
                         ? QStringLiteral("已读取当前账号的任务/活动缓存，可手动刷新")
                         : QStringLiteral("当前账号尚无任务/活动状态缓存"));
}

void RoutineOverviewController::loadCache() {
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) return;
  const QString path = QDir(QFileInfo(repository_->cachePath()).absolutePath())
                           .filePath(QStringLiteral("routines.json"));
  const QJsonObject root = readObject(path);
  const int schema = root.value(QStringLiteral("schema")).toInt();
  if ((schema != 1 && schema != 2 && schema != 3) ||
      root.value(QStringLiteral("account")).toString() != account_)
    return;
  if (root.value(QStringLiteral("dailyPacket")).isObject()) {
    dailyPacket_ = root.value(QStringLiteral("dailyPacket")).toObject();
    hasDailyPacket_ = root.value(QStringLiteral("hasDailyPacket")).toBool();
  }
  if (root.value(QStringLiteral("redPoints")).isArray()) {
    for (const QJsonValue& value : root.value(QStringLiteral("redPoints")).toArray())
      activeRedPoints_.insert(value.toInt());
    hasRedPointPacket_ = root.value(QStringLiteral("hasRedPointPacket")).toBool();
  }
  if (schema == 3 && root.value(QStringLiteral("opportunityPackets")).isObject())
    opportunityPackets_ = root.value(QStringLiteral("opportunityPackets")).toObject();
  else if (root.value(QStringLiteral("opportunityPacket")).isObject() &&
           root.value(QStringLiteral("hasOpportunityPacket")).toBool())
    opportunityPackets_.insert(kStarWheelCommand,
                               root.value(QStringLiteral("opportunityPacket")).toObject());
}

void RoutineOverviewController::saveCache() const {
  if (!repository_ || account_.isEmpty()) return;
  QJsonArray points;
  QList<int> sorted = activeRedPoints_.values();
  std::sort(sorted.begin(), sorted.end());
  for (int point : sorted) points.append(point);
  const QString path = QDir(QFileInfo(repository_->cachePath()).absolutePath())
                           .filePath(QStringLiteral("routines.json"));
  writeObject(path, {{QStringLiteral("schema"), 3},
                     {QStringLiteral("account"), account_},
                     {QStringLiteral("savedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                     {QStringLiteral("hasDailyPacket"), hasDailyPacket_},
                     {QStringLiteral("dailyPacket"), dailyPacket_},
                     {QStringLiteral("hasRedPointPacket"), hasRedPointPacket_},
                     {QStringLiteral("redPoints"), points},
                     {QStringLiteral("opportunityPackets"), opportunityPackets_}});
}
