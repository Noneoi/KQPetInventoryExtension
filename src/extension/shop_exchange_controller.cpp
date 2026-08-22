#include "shop_exchange_controller.h"

#include "diagnostic_logger.h"
#include "pet_repository.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QTimer>
#include <QJsonArray>
#include <QSet>

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

}  // namespace

ShopExchangeController::ShopExchangeController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  timeout_ = new QTimer(this);
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this]() {
    const bool partial = shopResponseReceived_ || materialResponseReceived_;
    finish(partial,
           partial ? QStringLiteral("部分查询超时：已保存成功返回的数据，另一部分保留旧缓存")
                   : QStringLiteral("兑换次数与商店货币查询超时，已保留旧缓存"));
  });
  if (repository_) {
    account_ = repository_->accountKey();
    sessionGeneration_ = repository_->sessionGeneration();
    connect(repository_, &PetRepository::accountSessionChanged, this,
            &ShopExchangeController::changeSession);
    loadCache();
    ShopExchangeCatalog::instance().reloadFromDataRoot(repository_->dataRoot());
  }
}

void ShopExchangeController::setSender(Sender sender) { sender_ = std::move(sender); }

bool ShopExchangeController::requestInfo() {
  const ShopExchangeCatalog& catalog = ShopExchangeCatalog::instance();
  if (!catalog.isLoaded()) {
    emit statusChanged(QStringLiteral("商店兑换配置未加载"));
    return false;
  }
  if (!sender_) {
    emit statusChanged(QStringLiteral("原版发送入口尚未就绪"));
    return false;
  }
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) {
    emit statusChanged(QStringLiteral("请先登录并识别账号，再查询兑换次数"));
    return false;
  }
  if (running_) return false;
  requestAccount_ = account_;
  requestSessionGeneration_ = sessionGeneration_;
  running_ = true;
  DiagnosticLogger::info(QStringLiteral("shop"),
                         QStringLiteral("refresh started session_generation=%1")
                             .arg(requestSessionGeneration_));
  shopResponseReceived_ = false;
  materialResponseReceived_ = false;
  emit runningChanged(true);
  emit statusChanged(QStringLiteral("正在手动查询六个商店的兑换次数与账号货币……"));
  const bool shopSent =
      sender_(catalog.extension(), catalog.getInfoCommand(), catalog.getInfoParams());
  const bool materialSent = sender_(QStringLiteral("MaterialExtension"),
                                    QStringLiteral("3_11"), QStringLiteral("{}"));
  if (!shopSent && !materialSent) {
    finish(false, QStringLiteral("发送兑换次数与货币查询失败"));
    return false;
  }
  timeout_->start(10000);
  return true;
}

bool ShopExchangeController::updateCatalog() {
  if (!repository_) {
    emit statusChanged(QStringLiteral("精灵仓库尚未初始化"));
    return false;
  }
  QString error;
  if (!ShopExchangeCatalog::instance().updateFromOfficialData(repository_->dataRoot(),
                                                               &error)) {
    emit statusChanged(QStringLiteral("兑换项目更新失败，已保留旧配置：%1").arg(error));
    return false;
  }
  const ShopExchangeCatalog& catalog = ShopExchangeCatalog::instance();
  emit catalogUpdated();
  emit statusChanged(QStringLiteral("已从官方解包更新兑换项目和适用精灵（%1）")
                         .arg(catalog.sourceUpdatedAt().isValid()
                                  ? catalog.sourceUpdatedAt().toString(
                                        QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                  : QStringLiteral("时间未知")));
  return true;
}

void ShopExchangeController::handlePacket(const QString& method, const QString& payload) {
  if (method != QStringLiteral("recivedata")) return;
  QJsonParseError error{};
  QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    const int begin = payload.indexOf(QLatin1Char('{'));
    const int end = payload.lastIndexOf(QLatin1Char('}'));
    if (begin < 0 || end <= begin) return;
    document = QJsonDocument::fromJson(payload.mid(begin, end - begin + 1).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
  }
  const QJsonObject packet = document.object();
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  const bool shopResponse =
      command == ShopExchangeCatalog::instance().getInfoCommand();
  const bool materialResponse = command == QStringLiteral("3_11");
  if (!shopResponse && !materialResponse) return;
  if (!running_) return;
  if (!repository_ || !repository_->isAuthenticated() ||
      requestAccount_ != account_ || requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration()) {
    finish(false, QStringLiteral("账号已切换，已忽略旧账号的商店响应"));
    return;
  }
  if (packet.contains(QStringLiteral("r")) && packet.value(QStringLiteral("r")).toInt() != 1) {
    if (running_)
      finish(false, QStringLiteral("兑换次数查询被服务器拒绝"));
    return;
  }
  if (shopResponse) {
    packet_ = packet;
    hasPacket_ = true;
    shopResponseReceived_ = true;
  } else {
    materialCounts_ = parseRequiredMaterialCounts(packet);
    hasMaterialCounts_ = true;
    materialResponseReceived_ = true;
  }
  saveCache();
  if (shopResponseReceived_ && materialResponseReceived_) {
    finish(true, QStringLiteral("已更新六个商店的兑换次数与账号货币"));
  } else {
    emit infoUpdated();
  }
}

void ShopExchangeController::finish(bool ok, const QString& status) {
  timeout_->stop();
  running_ = false;
  if (ok)
    DiagnosticLogger::info(QStringLiteral("shop"),
                           QStringLiteral("refresh completed: %1").arg(status));
  else
    DiagnosticLogger::error(QStringLiteral("shop"),
                            QStringLiteral("refresh failed: %1").arg(status));
  emit runningChanged(false);
  emit statusChanged(status);
  if (ok) emit infoUpdated();
}

void ShopExchangeController::changeSession(const QString& account, quint64 generation) {
  timeout_->stop();
  const bool wasRunning = running_;
  running_ = false;
  requestAccount_.clear();
  requestSessionGeneration_ = 0;
  account_ = account;
  sessionGeneration_ = generation;
  packet_ = {};
  hasPacket_ = false;
  materialCounts_.clear();
  hasMaterialCounts_ = false;
  shopResponseReceived_ = false;
  materialResponseReceived_ = false;
  loadCache();
  if (wasRunning) emit runningChanged(false);
  emit infoUpdated();
  emit statusChanged(hasPacket_
                         ? QStringLiteral("已读取当前账号的商店兑换次数缓存，可手动刷新")
                         : QStringLiteral("当前账号尚无商店兑换次数缓存"));
}

void ShopExchangeController::loadCache() {
  packet_ = {};
  hasPacket_ = false;
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) return;
  const QString path = QDir(QFileInfo(repository_->cachePath()).absolutePath())
                           .filePath(QStringLiteral("shops.json"));
  const QJsonObject envelope = readObject(path);
  const int schema = envelope.value(QStringLiteral("schema")).toInt();
  if ((schema != 1 && schema != 2) ||
      envelope.value(QStringLiteral("account")).toString() != account_ ||
      !envelope.value(QStringLiteral("packet")).isObject())
    return;
  packet_ = envelope.value(QStringLiteral("packet")).toObject();
  hasPacket_ = !packet_.isEmpty();
  if (schema >= 2 && envelope.value(QStringLiteral("materialCounts")).isObject()) {
    const QJsonObject counts = envelope.value(QStringLiteral("materialCounts")).toObject();
    for (auto iterator = counts.begin(); iterator != counts.end(); ++iterator)
      materialCounts_.insert(iterator.key(), iterator.value().toVariant().toLongLong());
    hasMaterialCounts_ = envelope.value(QStringLiteral("hasMaterialCounts")).toBool();
  }
}

void ShopExchangeController::saveCache() const {
  if (!repository_ || (!hasPacket_ && !hasMaterialCounts_) || account_.isEmpty()) return;
  const QString path = QDir(QFileInfo(repository_->cachePath()).absolutePath())
                           .filePath(QStringLiteral("shops.json"));
  QJsonObject counts;
  for (auto iterator = materialCounts_.cbegin(); iterator != materialCounts_.cend(); ++iterator)
    counts.insert(iterator.key(), static_cast<double>(iterator.value()));
  writeObject(path, {{QStringLiteral("schema"), 2},
                     {QStringLiteral("account"), account_},
                     {QStringLiteral("savedAt"),
                      QDateTime::currentDateTime().toString(Qt::ISODate)},
                     {QStringLiteral("packet"), packet_},
                     {QStringLiteral("hasMaterialCounts"), hasMaterialCounts_},
                     {QStringLiteral("materialCounts"), counts}});
}

QHash<QString, qint64> ShopExchangeController::parseRequiredMaterialCounts(
    const QJsonObject& packet) const {
  QSet<QString> required;
  for (const ShopExchangeGood& good : ShopExchangeCatalog::instance().onlineGoods()) {
    QString cost = good.cost;
    cost.replace(QLatin1Char('|'), QLatin1Char('#'));
    for (const QString& part : cost.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
      const QStringList fields = part.split(QLatin1Char(':'));
      if (fields.size() >= 2) required.insert(fields.at(0) + QLatin1Char(':') + fields.at(1));
    }
  }
  QHash<QString, qint64> result;
  for (const QString& key : required) result.insert(key, 0);
  for (auto iterator = packet.begin(); iterator != packet.end(); ++iterator) {
    bool typeOk = false;
    const int type = iterator.key().toInt(&typeOk);
    if (!typeOk || !iterator.value().isArray()) continue;
    for (const QJsonValue& value : iterator.value().toArray()) {
      const QJsonObject material = value.toObject();
      const int id = material.value(QStringLiteral("i")).toInt();
      const QString key = QStringLiteral("%1:%2").arg(type).arg(id);
      if (!required.contains(key)) continue;
      result.insert(key, material.value(QStringLiteral("n")).toVariant().toLongLong());
    }
  }
  return result;
}
