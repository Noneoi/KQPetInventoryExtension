// Material inventory: the standalone material-backpack refresh,
// normal material and idle source-beast packets, and their cache.
// Part of the shop_exchange_controller implementation; see shop_exchange_controller.cpp for the rest.

#include "shop_exchange_controller.h"
#include "shop_exchange_controller_internal.h"

#include "diagnostics/diagnostic_logger.h"
#include "application/common/controller_cache_storage.h"
#include "application/catalog/catalog_io_service.h"
#include "application/pet/pet_repository.h"
#include "domain/activity_shop_observation.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QJsonArray>
#include <QSet>

#include <limits>
#include <utility>

using ShopExchangeInternal::decimalKey;
using ShopExchangeInternal::successfulReply;

QHash<QString, qint64> ShopExchangeController::materialCounts() const {
  QHash<QString, qint64> current;
  for (auto value = materialCounts_.begin(); value != materialCounts_.end(); ++value) {
    const QString group = QStringLiteral("material:") + value.key().section(QLatin1Char(':'), 0, 0);
    const auto state = fieldState(group);
    if ((state == PacketFieldState::Value || state == PacketFieldState::Empty) &&
        freshness_.observation(group).state != ObservationValidityState::Invalidated)
      current.insert(value.key(), value.value());
  }
  return current;
}

MaterialInventorySnapshot ShopExchangeController::cultivationMaterialInventory() const {
  auto result = cultivationMaterials_;
  result.running = materialsOnly_ && running_;
  return result;
}

bool ShopExchangeController::requestCultivationMaterials() {
  if (running_) {
    emit statusChanged(QStringLiteral("当前查询尚未结束，请稍后刷新材料数量"));
    return false;
  }
  if (!sender_ && !asyncSender_) {
    emit statusChanged(QStringLiteral("原版发送入口尚未就绪，保留本地材料数量"));
    return false;
  }
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) {
    emit statusChanged(QStringLiteral("请先登录并识别账号，再刷新材料数量"));
    return false;
  }
  requestAccount_ = account_;
  requestSessionGeneration_ = sessionGeneration_;
  materialsOnly_ = running_ = true;
  anyUpdated_ = anyReadOnly_ = false;
  revokeAsyncRequests();
  requestWarnings_.clear();
  activityReads_.clear(); activeActivity_ = {};
  pendingCommands_ = {QStringLiteral("3_11"), QStringLiteral("2_32_0")};
  emit runningChanged(true);
  emit infoUpdated();
  emit statusChanged(QStringLiteral("正在刷新材料背包……"));
  startingRequests_ = true;
  const auto sendRead = [this](const QString& extension, const QString& command, const QString& parameters) {
    if (asyncSender_) return queueRequest(extension, command, parameters);
    requests_.markDispatchedNow(command);
    return sender_(extension, command, parameters);
  };
  const bool materialSent = sendRead(QStringLiteral("MaterialExtension"), QStringLiteral("3_11"), QStringLiteral("{}"));
  if (!materialSent) completeRequest(QStringLiteral("3_11"), false, {QStringLiteral("元魂与精华背包请求发送失败")});
  const bool sourceSent = sendRead(QStringLiteral("PJXExtension"), QStringLiteral("2_32_0"), QStringLiteral("null"));
  if (!sourceSent) completeRequest(QStringLiteral("2_32_0"), false, {QStringLiteral("源兽仓库请求发送失败")});
  startingRequests_ = false;
  if (pendingCommands_.isEmpty())
    finish(anyUpdated_, requestWarnings_.isEmpty() ? QStringLiteral("材料背包已刷新")
                                                  : requestWarnings_.join(QStringLiteral("；")));
  else timeout_->start(asyncSender_ ? 10 : 10000);
  return materialSent || sourceSent;
}

bool ShopExchangeController::acceptCultivationMaterialPacket(const QJsonObject& packet, QStringList* warnings) {
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty() ||
      account_ != repository_->accountKey() || sessionGeneration_ != repository_->sessionGeneration() ||
      !successfulReply(packet)) return false;
  bool updated = false;
  for (const int type : {4}) {
    const QString group = QString::number(type);
    if (!packet.contains(group)) {
      if (warnings) warnings->append(QStringLiteral("%1数量未返回").arg(type == 4 ? QStringLiteral("元魂与精华") : QStringLiteral("源兽")));
      continue;
    }
    const auto raw = packet.value(group);
    bool valid = raw.isArray();
    QHash<QString, qint64> candidate;
    for (const auto& value : raw.toArray()) {
      qint64 id = 0, count = 0;
      if (!value.isObject() || !PacketContracts::checkedInteger(value.toObject().value(QStringLiteral("i")), &id,
            1, std::numeric_limits<int>::max()) ||
          !PacketContracts::checkedInteger(value.toObject().value(QStringLiteral("n")), &count, 0)) {
        valid = false;
        break;
      }
      const auto key = cultivationMaterialKey(type, int(id));
      if (candidate.contains(key)) { valid = false; break; }
      candidate.insert(key, count);
    }
    if (!valid) {
      if (warnings) warnings->append(QStringLiteral("%1数量无效，保留旧数量").arg(
          type == 4 ? QStringLiteral("元魂与精华") : QStringLiteral("源兽")));
      continue;
    }
    const auto prefix = group + QLatin1Char(':');
    for (auto old = cultivationMaterials_.counts.begin(); old != cultivationMaterials_.counts.end();) {
      if (old.key().startsWith(prefix)) old = cultivationMaterials_.counts.erase(old);
      else ++old;
    }
    for (auto value = candidate.cbegin(); value != candidate.cend(); ++value)
      cultivationMaterials_.counts.insert(value.key(), value.value());
    cultivationMaterials_.knownTypes.insert(type);
    const auto observed = QDateTime::currentDateTimeUtc();
    cultivationMaterials_.observedTimes.insert(type, observed);
    cultivationMaterials_.observedAt = observed;
    updated = true;
  }
  if (updated && cultivationMaterialStorage_) cultivationMaterialStorage_->save();
  return updated;
}

bool ShopExchangeController::acceptSourceBeastInventoryPacket(const QJsonObject& packet, QStringList* warnings) {
  if (!materialsOnly_ || !running_ || !pendingCommands_.contains(QStringLiteral("2_32_0")) ||
      !repository_ || !repository_->isAuthenticated() || account_.isEmpty() ||
      account_ != repository_->accountKey() || sessionGeneration_ != repository_->sessionGeneration() ||
      !successfulReply(packet)) return false;
  const auto decoded = PacketContracts::decodeSourceBeastInventory(packet);
  if (!decoded.valid()) { if (warnings) warnings->append(decoded.error); return false; }
  for (auto old = cultivationMaterials_.counts.begin(); old != cultivationMaterials_.counts.end();) {
    if (old.key().startsWith(QStringLiteral("24:"))) old = cultivationMaterials_.counts.erase(old);
    else ++old;
  }
  for (auto value = decoded.quantities.cbegin(); value != decoded.quantities.cend(); ++value) {
    cultivationMaterials_.counts.insert(cultivationMaterialKey(24, value.key()), value.value());
    // The official stage-up selection filters only sourceId, and then submits
    // each selected source's actual level. All warehouse levels are eligible;
    // the generic material cost's extra=1 is not a level-1 inventory filter.
    cultivationMaterials_.counts.insert(cultivationMaterialKey(24, value.key(), 1), value.value());
  }
  cultivationMaterials_.knownTypes.insert(24);
  cultivationMaterials_.knownExtraGroups.insert(QStringLiteral("24:1"));
  const auto observed = QDateTime::currentDateTimeUtc();
  cultivationMaterials_.observedTimes.insert(24, observed);
  cultivationMaterials_.observedAt = observed;
  if (cultivationMaterialStorage_) cultivationMaterialStorage_->save();
  return true;
}

QJsonObject ShopExchangeController::cultivationMaterialCacheObject() const {
  QJsonObject counts, times;
  QJsonArray knownTypes, knownExtraGroups;
  for (auto value = cultivationMaterials_.counts.cbegin(); value != cultivationMaterials_.counts.cend(); ++value)
    counts.insert(value.key(), QString::number(value.value()));
  for (const auto type : cultivationMaterials_.knownTypes) knownTypes.append(type);
  for (const auto& group : cultivationMaterials_.knownExtraGroups) knownExtraGroups.append(group);
  for (auto value = cultivationMaterials_.observedTimes.cbegin(); value != cultivationMaterials_.observedTimes.cend(); ++value)
    times.insert(QString::number(value.key()), value.value().toUTC().toString(Qt::ISODateWithMs));
  return {{QStringLiteral("schema"), 2}, {QStringLiteral("account"), account_},
      {QStringLiteral("sourceInventory"), QStringLiteral("2_32_0.eps-warehouse")},
      {QStringLiteral("counts"), counts}, {QStringLiteral("knownTypes"), knownTypes},
      {QStringLiteral("knownExtraGroups"), knownExtraGroups}, {QStringLiteral("observedTimes"), times},
      {QStringLiteral("observedAt"), cultivationMaterials_.observedAt.toUTC().toString(Qt::ISODateWithMs)}};
}

bool ShopExchangeController::applyCultivationMaterialCache(const QJsonObject& root) {
  qint64 schema = 0;
  if (!PacketContracts::checkedInteger(root.value(QStringLiteral("schema")), &schema, 1, 2) ||
      root.value(QStringLiteral("account")).toString() != account_ ||
      !root.value(QStringLiteral("counts")).isObject() || !root.value(QStringLiteral("knownTypes")).isArray() ||
      !root.value(QStringLiteral("knownExtraGroups")).isArray() || !root.value(QStringLiteral("observedTimes")).isObject()) return false;
  MaterialInventorySnapshot candidate;
  const bool sourceInventoryKnown = schema == 2 &&
      root.value(QStringLiteral("sourceInventory")).toString() == QStringLiteral("2_32_0.eps-warehouse");
  for (const auto& value : root.value(QStringLiteral("knownTypes")).toArray()) {
    qint64 type = 0;
    if (!PacketContracts::checkedInteger(value, &type) || (type != 4 && type != 24)) return false;
    candidate.knownTypes.insert(int(type));
  }
  for (const auto& value : root.value(QStringLiteral("knownExtraGroups")).toArray()) {
    if (value.toString() != QStringLiteral("24:1") || !candidate.knownTypes.contains(24)) return false;
    candidate.knownExtraGroups.insert(value.toString());
  }
  const auto times = root.value(QStringLiteral("observedTimes")).toObject();
  for (const auto type : candidate.knownTypes) {
    const auto time = QDateTime::fromString(times.value(QString::number(type)).toString(), Qt::ISODateWithMs);
    if (!time.isValid()) return false;
    candidate.observedTimes.insert(type, time.toUTC());
  }
  const auto counts = root.value(QStringLiteral("counts")).toObject();
  for (auto value = counts.constBegin(); value != counts.constEnd(); ++value) {
    const auto fields = value.key().split(QLatin1Char(':'));
    qint64 count = 0;
    if ((fields.size() != 2 && fields.size() != 3) || !decimalKey(fields[0]) || !decimalKey(fields[1]) ||
        !candidate.knownTypes.contains(fields[0].toInt()) ||
        (fields.size() == 3 && (fields[0] != QStringLiteral("24") || fields[2] != QStringLiteral("1") ||
            !candidate.knownExtraGroups.contains(QStringLiteral("24:1")))) ||
        !PacketContracts::checkedInteger(value.value(), &count, 0)) return false;
    candidate.counts.insert(value.key(), count);
  }
  if (candidate.knownExtraGroups.contains(QStringLiteral("24:1"))) {
    for (auto value = candidate.counts.cbegin(); value != candidate.counts.cend(); ++value) {
      const auto fields = value.key().split(QLatin1Char(':'));
      if (fields[0] != QStringLiteral("24")) continue;
      const auto counterpart = cultivationMaterialKey(24, fields[1].toInt(), fields.size() == 2 ? 1 : 0);
      if (!candidate.counts.contains(counterpart) || candidate.counts.value(counterpart) != value.value()) return false;
    }
  }
  for (const auto type : candidate.knownTypes) {
    // A network observation may arrive while the historical read is pending.
    // Its entire group, including an explicit empty group, wins over disk.
    if (cultivationMaterials_.observedTimes.contains(type) || (type == 24 && !sourceInventoryKnown)) continue;
    cultivationMaterials_.knownTypes.insert(type);
    cultivationMaterials_.observedTimes.insert(type, candidate.observedTimes.value(type));
    if (type == 24 && candidate.knownExtraGroups.contains(QStringLiteral("24:1")))
      cultivationMaterials_.knownExtraGroups.insert(QStringLiteral("24:1"));
    const auto prefix = QString::number(type) + QLatin1Char(':');
    for (auto value = candidate.counts.cbegin(); value != candidate.counts.cend(); ++value)
      if (value.key().startsWith(prefix)) cultivationMaterials_.counts.insert(value.key(), value.value());
  }
  for (const auto& time : cultivationMaterials_.observedTimes)
    if (!cultivationMaterials_.observedAt.isValid() || time > cultivationMaterials_.observedAt)
      cultivationMaterials_.observedAt = time;
  return true;
}

QSet<QString> ShopExchangeController::requiredMaterialTypes() const {
  QSet<QString> types;
  for (const ShopExchangeGood& good : ShopExchangeCatalog::instance().onlineGoods()) {
    QString cost = good.cost;
    cost.replace(QLatin1Char('|'), QLatin1Char('#'));
    for (const QString& part : cost.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
      const QStringList fields = part.split(QLatin1Char(':'));
      if (fields.size() >= 2 && decimalKey(fields[0]) && fields[0] != QStringLiteral("134"))
        types.insert(fields[0]);
    }
  }
  return types;
}

bool ShopExchangeController::acceptMaterialPacket(const QJsonObject& packet,
                                                   QStringList* warnings, QJsonObject* readOnly) {
  if (materialsOnly_) return acceptCultivationMaterialPacket(packet, warnings);
  const QSet<QString> required = requiredMaterialTypes();
  QSet<QString> present;
  bool updated = false;
  for (auto iterator = packet.begin(); iterator != packet.end(); ++iterator) {
    const QString type = iterator.key();
    // 3_11 also carries clothing, furniture and other inventories with their
    // own schemas. Only decode resource groups used by the displayed goods.
    if (!required.contains(type)) continue;
    present.insert(type);
    const QString stateKey = QStringLiteral("material:") + type;
    QHash<QString, qint64> candidate;
    bool valid = iterator.value().isArray();
    for (const QJsonValue& value : iterator.value().toArray()) {
      if (!value.isObject()) { valid = false; break; }
      const QJsonObject material = value.toObject();
      qint64 id = 0, count = 0;
      if (!PacketContracts::checkedInteger(material.value(QStringLiteral("i")), &id, 1,
                                            std::numeric_limits<int>::max()) ||
          !PacketContracts::checkedInteger(material.value(QStringLiteral("n")), &count, 0)) {
        valid = false;
        break;
      }
      const QString key = type + QLatin1Char(':') + QString::number(id);
      if (candidate.contains(key)) { valid = false; break; }
      candidate.insert(key, count);
    }
    // MoneyType.DIAMOND (8:2) is a virtual total in the official client. The
    // money inventory stores recharge diamonds (8:25) and given diamonds
    // (8:26); mirror MoneyService's own DIAMOND calculation here.
    if (valid && type == QStringLiteral("8")) {
      const qint64 recharge = candidate.value(QStringLiteral("8:25"), 0);
      const qint64 given = candidate.value(QStringLiteral("8:26"), 0);
      if (recharge > std::numeric_limits<qint64>::max() - given)
        valid = false;
      else
        candidate.insert(QStringLiteral("8:2"), recharge + given);
    }
    if (!valid) {
      if (!readOnly) fieldStates_.insert(stateKey, PacketFieldState::Invalid);
      warnings->append(QStringLiteral("材料类型%1数据无效").arg(type));
      continue;
    }
    if (readOnly) {
      QJsonObject group;
      for (auto item = candidate.begin(); item != candidate.end(); ++item)
        group.insert(item.key().section(QLatin1Char(':'), 1), QString::number(item.value()));
      readOnly->insert(type, group);
      updated = true;
      continue;
    }
    // Only this explicitly present array is replaced. Absent types retain
    // their prior observation; absent items are unknown, never invented zero.
    const QString prefix = type + QLatin1Char(':');
    for (auto old = materialCounts_.begin(); old != materialCounts_.end();) {
      if (old.key().startsWith(prefix)) old = materialCounts_.erase(old);
      else ++old;
    }
    for (auto value = candidate.begin(); value != candidate.end(); ++value)
      materialCounts_.insert(value.key(), value.value());
    fieldStates_.insert(stateKey, candidate.isEmpty() ? PacketFieldState::Empty
                                                     : PacketFieldState::Value);
    observeGroup(stateKey);
    updated = true;
  }
  for (const QString& type : required) {
    if (!present.contains(type)) {
      if (!readOnly) fieldStates_.insert(QStringLiteral("material:") + type, PacketFieldState::Missing);
      warnings->append(QStringLiteral("材料类型%1缺失").arg(type));
    }
  }
  // The public map contains only independently current groups. This flag
  // denotes usable observations, not a fabricated complete account balance.
  if (!readOnly) hasMaterialCounts_ = updated || !materialCounts().isEmpty();
  if (!updated && warnings->isEmpty()) warnings->append(QStringLiteral("材料分组缺失"));
  return updated;
}
