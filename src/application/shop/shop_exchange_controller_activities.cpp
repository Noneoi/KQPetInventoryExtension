// Activity exchanges: discovering supported activity sources, reading
// them one at a time and keeping their per-account observations.
// Part of the shop_exchange_controller implementation; see shop_exchange_controller.cpp for the rest.

#include "shop_exchange_controller.h"

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

namespace {

QJsonValue retainObservedFields(const QJsonValue& previous,const QJsonValue& latest,int depth = 0) {
  if (depth > 12) return latest;
  if (previous.isObject() && latest.isObject()) {
    auto result = previous.toObject(); const auto incoming = latest.toObject();
    for (auto field = incoming.begin(); field != incoming.end(); ++field)
      result.insert(field.key(),retainObservedFields(result.value(field.key()),field.value(),depth+1));
    return result;
  }
  if (previous.isArray() && latest.isArray()) {
    auto result = previous.toArray(); const auto incoming = latest.toArray();
    for (qsizetype i = 0; i < incoming.size(); ++i) {
      if (i < result.size()) result[i] = retainObservedFields(result.at(i),incoming.at(i),depth+1);
      else result.append(incoming.at(i));
    }
    return result;
  }
  return latest;
}

}  // namespace

void ShopExchangeController::collectActivityReads() {
  activityReads_.clear(); activeActivity_ = {};
  for (auto source = activityObservations_.begin(); source != activityObservations_.end(); ++source) {
    auto records = source.value().toObject();
    for (auto record = records.begin(); record != records.end(); ++record) {
      auto object = record.value().toObject(); object.insert(QStringLiteral("historical"),true); record.value() = object;
    }
    source.value() = records;
  }
  const auto snapshot = ShopExchangeCatalog::instance().snapshot();
  for (const auto& shop : snapshot->allShops) {
    if (shop.sourceKey.isEmpty()) continue;
    for (const auto& value : shop.observation.value(QStringLiteral("requests")).toArray()) {
      const auto request = value.toObject();
      const auto command = request.value(QStringLiteral("command")).toString();
      const auto parameters = request.value(QStringLiteral("params"));
      const auto encoded = parameters.isNull() ? QStringLiteral("null")
          : QString::fromUtf8(QJsonDocument(parameters.toObject()).toJson(QJsonDocument::Compact));
      const auto* contract = PacketContracts::find(command);
      if (!contract || contract->access != PacketAccess::Read ||
          !PacketContracts::validateOutbound(request.value(QStringLiteral("extension")).toString(),command,encoded)) {
        requestWarnings_.append(shop.name + QStringLiteral("的独立查询尚未接入")); continue;
      }
      if (activityReads_.size() >= 64) { requestWarnings_.append(QStringLiteral("活动查询数量过多，本次保留部分旧观察")); return; }
      activityReads_.append({shop.sourceKey,shop.name,request});
    }
  }
}

void ShopExchangeController::startNextActivityRead() {
  if (!running_ || materialsOnly_ || !pendingCommands_.isEmpty()) return;
  // Activities the server refused for this session are dropped from the queue
  // instead of being asked again on every refresh: eligibility cannot change
  // before the next session, and repeating the query only produced a warning.
  while (!activityReads_.isEmpty() && notApplicableActivities_.contains(activityReads_.first().sourceKey)) {
    const auto skipped = activityReads_.takeFirst();
    emit statusChanged(QStringLiteral("%1 当前不适用（服务器已拒绝），本次跳过")
                           .arg(skipped.sourceName));
  }
  if (activityReads_.isEmpty()) return;
  activeActivity_ = activityReads_.takeFirst();
  const auto request = activeActivity_.request;
  const auto command = request.value(QStringLiteral("command")).toString();
  const auto parameters = request.value(QStringLiteral("params"));
  const auto encoded = parameters.isNull() ? QStringLiteral("null")
      : QString::fromUtf8(QJsonDocument(parameters.toObject()).toJson(QJsonDocument::Compact));
  pendingCommands_.insert(command); startingRequests_ = true;
  emit statusChanged(QStringLiteral("正在读取%1的次数与资源……").arg(activeActivity_.sourceName));
  bool sent = false;
  if (asyncSender_) sent = queueRequest(request.value(QStringLiteral("extension")).toString(),command,encoded);
  else if (sender_) {
    requests_.markDispatchedNow(command);
    sent = sender_(request.value(QStringLiteral("extension")).toString(),command,encoded);
  }
  if (!sent) completeRequest(command,false,{activeActivity_.sourceName + QStringLiteral("查询未提交")});
  startingRequests_ = false;
  if (pendingCommands_.isEmpty()) continueRequests();
  else timeout_->start(asyncSender_ ? 10 : 10000);
}

bool ShopExchangeController::acceptActivityPacket(const QJsonObject& packet,QStringList* warnings,bool verified) {
  if (!running_ || activeActivity_.sourceKey.isEmpty() || !repository_ ||
      requestAccount_ != repository_->accountKey() || requestSessionGeneration_ != repository_->sessionGeneration()) return false;
  const auto fail = [&] { if (warnings) warnings->append(activeActivity_.sourceName + QStringLiteral("数据不完整，保留旧观察")); return false; };
  if (QJsonDocument(packet).toJson(QJsonDocument::Compact).size() > 128 * 1024) return fail();
  for (const auto& field : activeActivity_.request.value(QStringLiteral("requiredFields")).toArray())
    if (!field.isString() || !packet.contains(field.toString()) || packet.value(field.toString()).isNull()) return fail();
  auto records = activityObservations_.value(activeActivity_.sourceKey).toObject();
  const auto previous = records.value(activeActivity_.request.value(QStringLiteral("key")).toString()).toObject();
  records.insert(activeActivity_.request.value(QStringLiteral("key")).toString(),QJsonObject{
      {QStringLiteral("request"),activityRequestSignature(activeActivity_.request)},
      {QStringLiteral("data"),packet},{QStringLiteral("observedAt"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {QStringLiteral("historical"),false},{QStringLiteral("verified"),verified}});
  if (previous.value(QStringLiteral("request")).toString() == activityRequestSignature(activeActivity_.request)) {
    const auto retained = retainObservedFields(previous.value(QStringLiteral("previousData")),previous.value(QStringLiteral("data"))).toObject();
    if (QJsonDocument(retained).toJson(QJsonDocument::Compact).size() <= 128 * 1024) {
      const auto key = activeActivity_.request.value(QStringLiteral("key")).toString(); auto record = records.value(key).toObject();
      record.insert(QStringLiteral("previousData"),retained);
      record.insert(QStringLiteral("previousObservedAt"),previous.value(QStringLiteral("previousObservedAt")).isString()
          ? previous.value(QStringLiteral("previousObservedAt")) : previous.value(QStringLiteral("observedAt")));
      records.insert(key,record);
    }
  }
  auto candidate = activityObservations_; candidate.insert(activeActivity_.sourceKey,records);
  const QJsonObject view{{QStringLiteral("_activities"),candidate}};
  const auto key = activeActivity_.request.value(QStringLiteral("key")).toString();
  for (const auto& shop : ShopExchangeCatalog::instance().snapshot()->allShops) {
    if (shop.sourceKey != activeActivity_.sourceKey) continue;
    for (const auto& good : shop.goods) {
      QList<QJsonObject> descriptors;
      if (good.quotaObservation.value(QStringLiteral("requestKey")).toString() == key) descriptors.append(good.quotaObservation);
      for (const auto& cost : good.activityCosts)
        if (cost.toObject().value(QStringLiteral("requestKey")).toString() == key) descriptors.append(cost.toObject());
      const auto appendCondition = [&descriptors,&key](const QJsonObject& condition) {
        if (condition.value(QStringLiteral("requestKey")).toString() == key) descriptors.append(condition);
        for (const auto& part : condition.value(QStringLiteral("all")).toArray())
          if (part.toObject().value(QStringLiteral("requestKey")).toString() == key) descriptors.append(part.toObject());
      };
      appendCondition(good.observationWhen);
      for (const auto& price : good.priceOptions) appendCondition(price.toObject().value(QStringLiteral("when")).toObject());
      for (const auto& descriptor : descriptors) {
        qint64 value = 0;
        const auto field = activityObservedValue(good,descriptor,view);
        if (!field.isUndefined() && !PacketContracts::checkedInteger(field,&value,0)) return fail();
      }
    }
  }
  activityObservations_.insert(activeActivity_.sourceKey,records);
  if (activityStorage_) activityStorage_->save();
  return true;
}

bool ShopExchangeController::applyActivityCache(const QJsonObject& object) {
  if (object.value(QStringLiteral("schema")).toInt() != 1 || object.value(QStringLiteral("account")).toString() != account_ ||
      !object.value(QStringLiteral("sources")).isObject()) return false;
  const auto sources = object.value(QStringLiteral("sources")).toObject();
  if (sources.size() > 256) return false;
  auto candidate = activityObservations_;
  for (auto source = sources.begin(); source != sources.end(); ++source) {
    if (source.key().size() > 512 || !source.value().isObject() || source.value().toObject().size() > 8) return false;
    auto records = candidate.value(source.key()).toObject();
    const auto saved = source.value().toObject();
    for (auto item = saved.begin(); item != saved.end(); ++item) {
      auto record = item.value().toObject();
      if (item.key().isEmpty() || item.key().size() > 64 || !record.value(QStringLiteral("data")).isObject() ||
          record.value(QStringLiteral("request")).toString().size() != 64 ||
          !QDateTime::fromString(record.value(QStringLiteral("observedAt")).toString(),Qt::ISODateWithMs).isValid()) return false;
      record.insert(QStringLiteral("historical"),true); record.insert(QStringLiteral("verified"),false);
      if (!records.contains(item.key())) records.insert(item.key(),record);
    }
    candidate.insert(source.key(),records);
  }
  activityObservations_ = std::move(candidate); return true;
}
