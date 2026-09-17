#include "shop_exchange_controller.h"

#include "diagnostic_logger.h"
#include "controller_cache_storage.h"
#include "catalog_io_service.h"
#include "pet_repository.h"
#include "../domain/activity_shop_observation.h"

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

bool decimalKey(const QString& key, qint64 minimum = 1) {
  qint64 value = 0;
  return PacketContracts::checkedInteger(key, &value, minimum,
                                         std::numeric_limits<int>::max()) &&
         QString::number(value) == key;
}

bool successfulReply(const QJsonObject& packet) {
  qint64 result = 0;
  return (!packet.contains(QStringLiteral("$")) || packet.value(QStringLiteral("$")).isNull()) && (!packet.contains(QStringLiteral("r")) ||
         (PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &result) &&
          result == 1));
}

// Why a response was not accepted as a successful one. The GUI used to report a
// bare "rejected or wrong result type" with no way to tell a stale activity
// aside from an unexpected success marker, so the exact field is named here and
// the bounded payload is recorded for offline comparison.
QString replyRejectionReason(const QJsonObject& packet) {
  qint64 result = 0;
  if (packet.contains(QStringLiteral("$")) && !packet.value(QStringLiteral("$")).isNull())
    return QStringLiteral("$=%1").arg(packet.value(QStringLiteral("$")).toVariant().toString());
  if (packet.contains(QStringLiteral("r"))) {
    const QJsonValue value = packet.value(QStringLiteral("r"));
    if (!PacketContracts::checkedInteger(value, &result, std::numeric_limits<qint64>::min(),
                                         std::numeric_limits<qint64>::max()))
      return QStringLiteral("r=%1 (not an integer)").arg(value.toVariant().toString());
    return QStringLiteral("r=%1").arg(result);
  }
  return QStringLiteral("no success marker");
}

QString boundedPayload(const QJsonObject& packet) {
  const QByteArray bytes = QJsonDocument(packet).toJson(QJsonDocument::Compact);
  return bytes.size() <= 512 ? QString::fromUtf8(bytes)
                             : QString::fromUtf8(bytes.left(512)) + QStringLiteral("…");
}

// A negative result code is a server refusal, not a malformed response: the
// activity exists but this account may not query it (return-player activities
// are gated by the official unlock flag). It carries no usable counters, so it
// is reported as "not applicable" and never stored.
bool serverRefusal(const QJsonObject& packet, qint64* code) {
  qint64 result = 0;
  if (packet.contains(QStringLiteral("$")) && !packet.value(QStringLiteral("$")).isNull()) return false;
  if (!packet.contains(QStringLiteral("r"))) return false;
  if (!PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &result)) return false;
  if (result >= 0) return false;
  if (code) *code = result;
  return true;
}

bool validShopGroup(const QJsonObject& shop) {
  // Captured siN objects also contain unrelated progress and reward metadata.
  // Validate the consumed per-item counters without interpreting that metadata.
  for (auto item = shop.begin(); item != shop.end(); ++item) {
    QString id;
    if (item.key().startsWith(QStringLiteral("bi"))) id = item.key().mid(2);
    else if (item.key().startsWith(QLatin1Char('b'))) id = item.key().mid(1);
    else continue;
    if (!decimalKey(id, 0) || !item.value().isObject()) return false;
    const QJsonObject counters = item.value().toObject();
    for (const QString& key : {QStringLiteral("dl"), QStringLiteral("wl"),
                               QStringLiteral("ml"), QStringLiteral("pl"),
                               QStringLiteral("tl")}) {
      if (!counters.contains(key)) continue;
      qint64 count = 0;
      if (!PacketContracts::checkedInteger(counters.value(key), &count, 0,
                                           std::numeric_limits<int>::max())) return false;
    }
    const QString alias = item.key().startsWith(QStringLiteral("bi"))
                              ? QStringLiteral("b") + id : QStringLiteral("bi") + id;
    if (shop.contains(alias) && shop.value(alias) != item.value()) return false;
  }
  return true;
}

}  // namespace

ShopExchangeController::ShopExchangeController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  timeout_ = new QTimer(this);
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this]() {
    if (asyncSender_) { pollAsyncRequests(); return; }
    if (!activeActivity_.sourceKey.isEmpty()) {
      completeRequest(activeActivity_.request.value(QStringLiteral("command")).toString(),false,
          {activeActivity_.sourceName + QStringLiteral("查询超时")},true); return;
    }
    if (!activityReads_.isEmpty()) {
      const auto pending = pendingCommands_;
      startingRequests_ = true;
      for (const auto& command : pending) completeRequest(command,false,{command + QStringLiteral("响应超时")});
      startingRequests_ = false; continueRequests(); return;
    }
    for (const auto& command : pendingCommands_)
      requestWarnings_.append(QStringLiteral("%1响应超时").arg(command == QStringLiteral("2_32_0") ? QStringLiteral("源兽仓库") : command));
    finish(anyUpdated_, anyUpdated_
           ? QStringLiteral("部分查询超时：已接收成功返回的数据，另一部分保留旧缓存")
           : materialsOnly_ ? QStringLiteral("材料背包查询超时，已保留旧数量")
                            : QStringLiteral("兑换次数与商店货币查询超时，已保留旧缓存"));
  });
  if (repository_) {
    initializeCacheStorage();
    account_ = repository_->accountKey();
    sessionGeneration_ = repository_->sessionGeneration();
    freshness_.bindSession(account_, sessionGeneration_);
    connect(repository_, &PetRepository::accountSessionChanged, this,
            &ShopExchangeController::changeSession);
    connect(repository_, &PetRepository::sessionTrustChanged, this,
        [this](SessionConnectionState, const QString&) { checkFreshness(); });
    loadCache();
  }
}

void ShopExchangeController::setSender(Sender sender) { sender_ = std::move(sender); }

void ShopExchangeController::setCatalogIoService(CatalogIoService* service) {
  if (catalogIo_) disconnect(catalogIo_, nullptr, this, nullptr);
  catalogIo_ = service;
  if (!service) return;
  connect(service, &CatalogIoService::catalogUpdated, this, [this](CatalogKind kind, quint64) {
    if (kind == CatalogKind::Shop) emit catalogUpdated();
  });
  connect(service, &CatalogIoService::finished, this,
      [this](quint64, CatalogKind kind, StorageStatus status, const QString& error) {
    if (kind != CatalogKind::Shop || status == StorageStatus::Cancelled || status == StorageStatus::NotFound) return;
    emit statusChanged(status == StorageStatus::Saved || status == StorageStatus::Loaded
        ? QStringLiteral("官方目录已更新")
        : QStringLiteral("目录未更新，继续使用上一版本：%1").arg(error));
  });
}


ShopExchangeController::~ShopExchangeController() { revokeAsyncRequests(); }

void ShopExchangeController::setObservationClock(ObservationClock clock) {
  freshness_.setClock(std::move(clock));
  if (!checkFreshness()) publishFreshness();
}
void ShopExchangeController::publishFreshness() {
  const QPointer<ShopExchangeController> alive(this);
  emit freshnessChanged();
  if (alive) emit infoUpdated();
}
void ShopExchangeController::observeGroup(const QString& group) {
  freshness_.observe(group, lastInboundSequence_, capturedMonotonicMs_);
  observedAt_.insert(group, freshness_.observedAt(group));
}
bool ShopExchangeController::checkFreshness() {
  const quint64 before = freshness_.revision();
  bool activityChanged = false;
  if (!repository_ || !repository_->sessionContext().canPersist()) {
    freshness_.invalidateObservations(QStringLiteral("会话来源未确认或已失效"));
    for (auto source = activityObservations_.begin(); source != activityObservations_.end(); ++source) {
      auto records = source.value().toObject();
      for (auto record = records.begin(); record != records.end(); ++record) {
        auto object = record.value().toObject();
        if (object.value(QStringLiteral("verified")).toBool() || (!repository_ || !repository_->isAuthenticated())) {
          if (!object.value(QStringLiteral("historical")).toBool(true) || object.value(QStringLiteral("verified")).toBool()) activityChanged = true;
          object.insert(QStringLiteral("verified"),false); object.insert(QStringLiteral("historical"),true); record.value() = object;
        }
      }
      source.value() = records;
    }
  }
  freshness_.check();
  if (before == freshness_.revision() && !activityChanged) return false;
  publishFreshness();
  return true;
}
bool ShopExchangeController::acceptQuotaValidityEvidence(const TrustedObservationValidity& evidence, QString* error) {
  const QPointer<ShopExchangeController> alive(this);
  checkFreshness();
  if (!alive) return false;
  const QStringList knownPeriods{QStringLiteral("dl"), QStringLiteral("wl"), QStringLiteral("ml"), QStringLiteral("pl"), QStringLiteral("tl")};
  if (!evidence.group.startsWith(QStringLiteral("si")) || !packet_.value(evidence.group).isObject() ||
      !knownPeriods.contains(evidence.periodKey) ||
      (fieldState(evidence.group) != PacketFieldState::Empty && fieldState(evidence.group) != PacketFieldState::Value)) {
    if (error) *error = QStringLiteral("周期证据不对应有效商店分组与已登记的配额键");
    return false;
  }
  if (!repository_ || !repository_->sessionContext().canPersist()) {
    if (error) *error = QStringLiteral("当前来源不可验证周期证据");
    return false;
  }
  const quint64 before = freshness_.revision();
  const bool accepted = freshness_.acceptValidityEvidence(evidence, error);
  if (before != freshness_.revision()) publishFreshness();
  return accepted;
}
QHash<QString, ShopCondition> ShopExchangeController::quotaValiditySnapshot() const {
  QHash<QString, ShopCondition> result;
  for (auto group = packet_.begin(); group != packet_.end(); ++group) {
    if (!group.key().startsWith(QStringLiteral("si")) || !group.value().isObject()) continue;
    for (const QString& period : {QStringLiteral("dl"), QStringLiteral("wl"), QStringLiteral("ml"), QStringLiteral("pl"), QStringLiteral("tl")}) {
      const auto validity = freshness_.status(group.key(), period);
      ShopCondition condition;
      condition.state = validity.current() ? ShopConditionState::Satisfied : ShopConditionState::Unknown;
      condition.freshness = validity.current() ? ShopConditionFreshness::Current
          : validity.state == ObservationValidityState::Invalidated ? ShopConditionFreshness::Invalidated : ShopConditionFreshness::Unknown;
      condition.reason = validity.reason; condition.source = validity.evidenceReference;
      condition.observedAt = validity.observedAtUtc;
      if (!repository_ || !repository_->sessionContext().canPersist() ||
          (fieldState(group.key()) != PacketFieldState::Empty && fieldState(group.key()) != PacketFieldState::Value)) {
        condition.state = ShopConditionState::Unknown;
        condition.freshness = ShopConditionFreshness::Invalidated;
        condition.reason = QStringLiteral("分组或来源未更新；保留只读旧观察");
      }
      result.insert(group.key() + QLatin1Char(':') + period, condition);
    }
  }
  return result;
}
QString ShopExchangeController::freshnessSummary() const {
  const auto values = quotaValiditySnapshot();
  bool known = false, stale = false;
  for (const auto& value : values) {
    known |= value.state == ShopConditionState::Satisfied && value.freshness == ShopConditionFreshness::Current;
    stale |= value.freshness == ShopConditionFreshness::Invalidated;
  }
  if (stale) return QStringLiteral("部分周期或时钟已失效；旧次数只读");
  return known ? QStringLiteral("已核验部分限次周期，其余保持待确认") : QStringLiteral("限次周期未核验；当前数值仅为只读观察");
}

bool ShopExchangeController::queueRequest(const QString& extension, const QString& command,
                                           const QString& parameters) {
  if (!asyncSender_ || !running_ || !repository_ || requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != repository_->sessionGeneration()) return false;
  for (const auto& intent : std::as_const(pendingSends_))
    if (intent.ticket.command == command) return true;
  OutboundIntent intent;
  intent.ticket = {nextTransportTaskId(), requestAccount_, requestSessionGeneration_, command, 0,
                   transportMonotonicMs() + 10000, 10000, PacketCorrelationStrength::CommandObserved};
  intent.source = repository_->sessionContext().source;
  intent.extension = extension; intent.parameters = parameters;
  pendingSends_.insert(intent.ticket.taskId, intent);
  if (asyncSender_(intent)) return true;
  intent.permit.revoke(); pendingSends_.remove(intent.ticket.taskId);
  return false;
}

void ShopExchangeController::revokeAsyncRequests() {
  for (const auto& intent : std::as_const(pendingSends_)) intent.permit.revoke();
  pendingSends_.clear(); responseDeadlines_.clear();
  requestDispatchTimes_.clear();
}

void ShopExchangeController::handleSendReceipt(const SendReceipt& receipt) {
  const auto found = pendingSends_.find(receipt.taskId);
  if (found == pendingSends_.end()) return;
  const OutboundIntent intent = found.value();
  if (receipt.account != intent.ticket.account || receipt.sessionEpoch != intent.ticket.sessionEpoch ||
      receipt.command != intent.ticket.command) return;
  pendingSends_.erase(found);
  if (!running_ || !pendingCommands_.contains(receipt.command) || !repository_ ||
      receipt.account != repository_->accountKey() || receipt.sessionEpoch != repository_->sessionGeneration()) return;
  if (receipt.outcome == SubmissionOutcome::DefinitelyNotSubmitted || receipt.dispatchedAtMs < 0) {
    completeRequest(receipt.command, false, {QStringLiteral("%1未提交：%2").arg(receipt.command, receipt.error)});
    return;
  }
  requestDispatchTimes_.insert(receipt.command, receipt.dispatchedAtMs);
  const qint64 deadline = receipt.dispatchedAtMs + intent.ticket.responseTimeoutMs;
  if (deadline <= transportMonotonicMs())
    completeRequest(receipt.command, false, {QStringLiteral("%1实际发送后的响应已超时").arg(receipt.command)},true);
  else responseDeadlines_.insert(receipt.command, deadline);
}

void ShopExchangeController::pollAsyncRequests() {
  QList<SendReceipt> receipts;
  const qint64 now = transportMonotonicMs();
  for (const auto& intent : std::as_const(pendingSends_)) {
    const auto receipt = pollSendReceipt(intent, now);
    if (receipt) receipts.append(*receipt);
  }
  for (const auto& receipt : receipts) handleSendReceipt(receipt);
  const auto deadlines = responseDeadlines_;
  for (auto deadline = deadlines.begin(); deadline != deadlines.end(); ++deadline)
    if (deadline.value() <= now)
      completeRequest(deadline.key(), false, {QStringLiteral("%1响应超时").arg(deadline.key())},true);
  if (running_) timeout_->start(10);
}

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

bool ShopExchangeController::requestInfo() {
  const ShopExchangeCatalog& catalog = ShopExchangeCatalog::instance();
  if (!catalog.isLoaded()) {
    emit statusChanged(QStringLiteral("商店兑换配置未加载"));
    return false;
  }
  if (!sender_ && !asyncSender_) {
    emit statusChanged(QStringLiteral("原版发送入口尚未就绪"));
    return false;
  }
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) {
    emit statusChanged(QStringLiteral("请先登录并识别账号，再查询兑换次数"));
    return false;
  }
  if (running_) return false;
  materialsOnly_ = false;
  requestAccount_ = account_;
  requestSessionGeneration_ = sessionGeneration_;
  running_ = true;
  DiagnosticLogger::info(QStringLiteral("shop"),
                         QStringLiteral("refresh started session_generation=%1")
                             .arg(requestSessionGeneration_));
  anyUpdated_ = false;
  anyReadOnly_ = false;
  revokeAsyncRequests();
  requestWarnings_.clear();
  collectActivityReads();
  pendingCommands_ = {catalog.getInfoCommand(), QStringLiteral("3_11")};
  hasPacket_ = false;
  hasMaterialCounts_ = false;
  for (const auto& shop : catalog.protocolShops())
    fieldStates_.insert(QStringLiteral("si%1").arg(shop.shopId), PacketFieldState::Missing);
  for (auto state = fieldStates_.begin(); state != fieldStates_.end(); ++state) {
    if (state.key().startsWith(QStringLiteral("material:")) &&
        state.key() != QStringLiteral("material:134")) state.value() = PacketFieldState::Missing;
  }
  for (const QString& type : requiredMaterialTypes())
    fieldStates_.insert(QStringLiteral("material:") + type, PacketFieldState::Missing);
  emit runningChanged(true);
  emit infoUpdated();
  emit statusChanged(QStringLiteral("正在查询兑换次数与所需资源……"));
  startingRequests_ = true;
  const auto sendRequest = [this](const QString& extension, const QString& command,
                                  const QString& parameters) {
    if (asyncSender_) return queueRequest(extension, command, parameters);
    requestDispatchTimes_.insert(command, transportMonotonicMs());
    return sender_(extension, command, parameters);
  };
  const bool shopSent = sendRequest(catalog.extension(), catalog.getInfoCommand(), catalog.getInfoParams());
  const bool materialSent = sendRequest(QStringLiteral("MaterialExtension"), QStringLiteral("3_11"), QStringLiteral("{}"));
  if (!shopSent) completeRequest(catalog.getInfoCommand(), false,
                                 {QStringLiteral("兑换次数请求发送失败")});
  if (!materialSent) completeRequest(QStringLiteral("3_11"), false,
                                     {QStringLiteral("货币请求发送失败")});
  startingRequests_ = false;
  if (pendingCommands_.isEmpty()) continueRequests();
  else timeout_->start(asyncSender_ ? 10 : 10000);
  return shopSent || materialSent || !activityReads_.isEmpty() || !activeActivity_.sourceKey.isEmpty();
}

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

MaterialInventorySnapshot ShopExchangeController::cultivationMaterialInventory() const {
  auto result = cultivationMaterials_;
  result.running = materialsOnly_ && running_;
  return result;
}

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

void ShopExchangeController::continueRequests() {
  if (!running_ || startingRequests_ || !pendingCommands_.isEmpty()) return;
  if (!materialsOnly_ && !activityReads_.isEmpty()) {
    timeout_->stop();
    const auto account = requestAccount_; const auto epoch = requestSessionGeneration_;
    QTimer::singleShot(0,this,[this,account,epoch] {
      if (running_ && requestAccount_ == account && requestSessionGeneration_ == epoch && pendingCommands_.isEmpty()) startNextActivityRead();
    });
    emit infoUpdated(); return;
  }
  finish(anyUpdated_,requestWarnings_.isEmpty()
      ? (materialsOnly_ ? QStringLiteral("材料背包已刷新") : QStringLiteral("兑换次数与所需资源已刷新"))
      : QStringLiteral("%1；%2，未更新部分保留旧缓存")
          .arg(anyUpdated_ ? QStringLiteral("已接收可用数据") : QStringLiteral("未更新数据"),requestWarnings_.join(QStringLiteral("；"))));
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
    requestDispatchTimes_.insert(command,transportMonotonicMs());
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
    requestDispatchTimes_.insert(command, transportMonotonicMs());
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

bool ShopExchangeController::updateCatalog() {
  if (!catalogIo_ || !catalogIo_->requestOfficialUpdate(CatalogKind::Shop)) {
    emit statusChanged(QStringLiteral("目录更新服务尚未就绪"));
    return false;
  }
  emit statusChanged(QStringLiteral("正在后台读取官方兑换目录"));
  return true;
}

void ShopExchangeController::handlePacket(const QString& method, const QString& payload) {
  QJsonObject packet;
  if (!PacketContracts::decodeObject(method, payload, &packet)) return;
  handleDecodedEnvelope({}, packet);
}

void ShopExchangeController::handleDecodedEnvelope(const InboundEnvelope& envelope,
                                                     const QJsonObject& packet) {
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  const bool activityResponse = !activeActivity_.sourceKey.isEmpty() &&
      activeActivity_.request.value(QStringLiteral("command")).toString() == command;
  if (command != ShopExchangeCatalog::instance().getInfoCommand() &&
      command != QStringLiteral("3_11") && command != QStringLiteral("2_32_0") && command != QStringLiteral("1015_2A") && !activityResponse) return;
  if (activityResponse && command == QStringLiteral("1019_0")) {
    qint64 actual = 0,expected = 0;
    if (!PacketContracts::checkedInteger(packet.value(QStringLiteral("ai")),&actual,1) ||
        !PacketContracts::checkedInteger(activeActivity_.request.value(QStringLiteral("params")).toObject().value(QStringLiteral("ai")),&expected,1) || actual != expected) return;
  }
  if (!repository_ || !repository_->sessionContext().accepts(envelope)) {
    if (!matchesReadOnlyRequest(envelope, command)) return;
    lastInboundSequence_ = envelope.receiveSequence;
    QStringList warnings;
    QJsonObject observation;
    bool updated = false;
    if (!successfulReply(packet)) {
      qint64 refusalCode = 0;
      const QString reason = replyRejectionReason(packet);
      if (activityResponse && serverRefusal(packet, &refusalCode)) {
        // The official unlock flag gates this activity and the account does not
        // satisfy it, so the server refuses the query. Nothing is stored and the
        // same session will not ask again; the wording says so instead of
        // reporting a parse failure on every refresh.
        notApplicableActivities_.insert(activeActivity_.sourceKey, activeActivity_.sourceName);
        DiagnosticLogger::info(QStringLiteral("response"),
                               QStringLiteral("activity not applicable command=%1 code=%2")
                                   .arg(command).arg(refusalCode));
        warnings.append(QStringLiteral("%1 当前不适用（服务器返回 %2）")
                            .arg(activeActivity_.sourceName).arg(refusalCode));
      } else {
        DiagnosticLogger::warning(QStringLiteral("response"),
                                  QStringLiteral("read-only reply rejected command=%1 reason=%2 payloadBytes=%3")
                                      .arg(command, reason).arg(QJsonDocument(packet).toJson(QJsonDocument::Compact).size()));
        warnings.append(QStringLiteral("%1响应被拒绝或结果类型错误（%2）").arg(command, reason));
      }
    }
    else if (activityResponse)
      updated = acceptActivityPacket(packet,&warnings,false);
    else if (command == QStringLiteral("3_11"))
      updated = acceptMaterialPacket(packet, &warnings, &observation);
    else if (command == QStringLiteral("2_32_0"))
      updated = acceptSourceBeastInventoryPacket(packet, &warnings);
    else updated = acceptShopPacket(packet, &warnings, &observation);
    if (updated) {
      if (!materialsOnly_ && !activityResponse) {
        QJsonObject retained = unverifiedPackets_.value(command).toObject();
        for (auto group = observation.begin(); group != observation.end(); ++group)
          retained.insert(group.key(), group.value());
        unverifiedPackets_.insert(command, retained);
      }
      anyReadOnly_ = true;
    }
    completeRequest(command, updated, warnings);
    return;
  }
  if (envelope.receiveSequence == 0 || envelope.receiveSequence <= lastInboundSequence_) return;
  lastInboundSequence_ = envelope.receiveSequence;
  capturedMonotonicMs_ = envelope.receivedMonotonicMs;
  if (activityResponse) {
    QStringList warnings;
    const bool updated = successfulReply(packet) && acceptActivityPacket(packet,&warnings,true);
    if (!successfulReply(packet)) {
      qint64 refusalCode = 0;
      if (serverRefusal(packet, &refusalCode)) {
        notApplicableActivities_.insert(activeActivity_.sourceKey, activeActivity_.sourceName);
        DiagnosticLogger::info(QStringLiteral("response"),
                               QStringLiteral("activity not applicable command=%1 code=%2")
                                   .arg(command).arg(refusalCode));
        warnings.append(QStringLiteral("%1 当前不适用（服务器返回 %2）")
                            .arg(activeActivity_.sourceName).arg(refusalCode));
      } else {
        DiagnosticLogger::warning(QStringLiteral("response"),
                                  QStringLiteral("activity reply rejected command=%1 reason=%2")
                                      .arg(command, replyRejectionReason(packet)));
        warnings.append(activeActivity_.sourceName + QStringLiteral("查询被拒绝"));
      }
    }
    completeRequest(command,updated,warnings); return;
  }
  if (!materialsOnly_) unverifiedPackets_.remove(command);
  acceptingVerifiedPacket_ = true;
  handleVerifiedPacket(packet);
  acceptingVerifiedPacket_ = false;
}

bool ShopExchangeController::matchesReadOnlyRequest(const InboundEnvelope& envelope,
                                                    const QString& command) const {
  if (!repository_ || !running_ || !pendingCommands_.contains(command) ||
      envelope.source.verified() || repository_->sessionContext().source.verified() ||
      repository_->sessionContext().state != SessionConnectionState::Uncertain ||
      !repository_->isAuthenticated() || requestAccount_ != repository_->accountKey() ||
      requestAccount_ != account_ || requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration() ||
      (envelope.capturedSessionEpoch != 0 && envelope.capturedSessionEpoch != requestSessionGeneration_) ||
      envelope.receiveSequence == 0 || envelope.receiveSequence <= lastInboundSequence_) return false;
  qint64 dispatched = requestDispatchTimes_.value(command, -1);
  // Production capture deliberately leaves the unknown host epoch at zero.
  // Only the locally scoped request and post-dispatch observation are matched;
  // this does not turn zero into verified session evidence.
  // The host may deliver a response before the queued send receipt reaches Core.
  // The shared permit still proves this request crossed its execution boundary.
  for (const auto& intent : pendingSends_) {
    if (intent.ticket.command != command) continue;
    const auto state = intent.permit.state();
    if (state == SendPermitState::Claimed || state == SendPermitState::Submitted ||
        state == SendPermitState::Unknown) dispatched = intent.permit.dispatchedAtMs();
  }
  return dispatched >= 0 && envelope.receivedMonotonicMs >= dispatched;
}

void ShopExchangeController::handleVerifiedPacket(const QJsonObject& packet) {
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  // Personal league contribution (material 134:1) is not part of the generic
  // MaterialExtension/3_11 response.  The game already requests this league
  // overview during its normal flow, so observe that response without adding
  // another server request.
  if (command == QStringLiteral("1015_2A")) {
    if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty() ||
        account_ != repository_->accountKey() ||
        sessionGeneration_ != repository_->sessionGeneration())
      return;
    const QJsonObject member = packet.value(QStringLiteral("infos"))
                                   .toObject()
                                   .value(QStringLiteral("UnionMemberInfo"))
                                   .toObject();
    qint64 contribution = 0;
    if (!successfulReply(packet) ||
        !PacketContracts::checkedInteger(member.value(QStringLiteral("lCToken")),
                                          &contribution, 0)) {
      fieldStates_.insert(QStringLiteral("material:134"), PacketFieldState::Invalid);
      emit infoUpdated();
      return;
    }
    materialCounts_.insert(QStringLiteral("134:1"), contribution);
    hasMaterialCounts_ = true;
    fieldStates_.insert(QStringLiteral("material:134"), PacketFieldState::Value);
    observeGroup(QStringLiteral("material:134"));
    saveCache();
    emit infoUpdated();
    return;
  }
  const bool shopResponse =
      command == ShopExchangeCatalog::instance().getInfoCommand();
  const bool materialResponse = command == QStringLiteral("3_11");
  const bool sourceResponse = command == QStringLiteral("2_32_0") && materialsOnly_;
  if (!shopResponse && !materialResponse && !sourceResponse) return;
  if (!running_ || !pendingCommands_.contains(command)) return;
  if (!repository_ || !repository_->isAuthenticated() ||
      requestAccount_ != account_ || requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration()) {
    finish(false, QStringLiteral("账号已切换，已忽略旧账号的商店响应"));
    return;
  }
  if (!successfulReply(packet)) {
    const QString reason = replyRejectionReason(packet);
    DiagnosticLogger::warning(QStringLiteral("response"),
                              QStringLiteral("reply rejected command=%1 reason=%2 payload=%3")
                                  .arg(command, reason, boundedPayload(packet)));
    completeRequest(command, false, {QStringLiteral("%1响应被拒绝或结果类型错误（%2）")
                                         .arg(shopResponse ? QStringLiteral("兑换次数")
                                                           : QStringLiteral("账号货币"), reason)});
    return;
  }
  QStringList warnings;
  const bool updated = shopResponse ? acceptShopPacket(packet, &warnings)
      : sourceResponse ? acceptSourceBeastInventoryPacket(packet, &warnings)
                       : acceptMaterialPacket(packet, &warnings);
  completeRequest(command, updated, warnings);
}

void ShopExchangeController::completeRequest(const QString& command, bool updated,
                                              const QStringList& warnings, bool skipSharedCommand) {
  if (!pendingCommands_.remove(command)) return;
  responseDeadlines_.remove(command);
  requestDispatchTimes_.remove(command);
  for (auto pending = pendingSends_.begin(); pending != pendingSends_.end();) {
    if (pending.value().ticket.command == command) {
      pending.value().permit.revoke(); pending = pendingSends_.erase(pending);
    } else ++pending;
  }
  requestWarnings_.append(warnings);
  if (!activeActivity_.sourceKey.isEmpty() && activeActivity_.request.value(QStringLiteral("command")).toString() == command) {
    if (skipSharedCommand) {
      for (auto pending = activityReads_.begin(); pending != activityReads_.end();) {
        if (pending->request.value(QStringLiteral("command")).toString() == command) {
          requestWarnings_.append(pending->sourceName + QStringLiteral("留待下次刷新"));
          pending = activityReads_.erase(pending);
        } else ++pending;
      }
    }
    activeActivity_ = {};
  }
  if (updated) { anyUpdated_ = true; if (!materialsOnly_) saveCache(); }
  if (pendingCommands_.isEmpty() && !startingRequests_) continueRequests();
  else emit infoUpdated();
}

void ShopExchangeController::finish(bool ok, const QString& status) {
  const QString displayStatus = anyReadOnly_
      ? (materialsOnly_ ? QStringLiteral("材料背包已更新") : QStringLiteral("只读商店数据已更新")) +
          (requestWarnings_.isEmpty() ? QString{} : QStringLiteral("；%1，未更新部分保留旧观察")
              .arg(requestWarnings_.join(QStringLiteral("；"))))
      : status;
  timeout_->stop();
  revokeAsyncRequests();
  pendingCommands_.clear();
  activityReads_.clear(); activeActivity_ = {};
  running_ = false;
  materialsOnly_ = false;
  if (ok)
    DiagnosticLogger::info(QStringLiteral("shop"),
                           QStringLiteral("refresh completed: %1").arg(displayStatus));
  else
    DiagnosticLogger::error(QStringLiteral("shop"),
                            QStringLiteral("refresh failed: %1").arg(displayStatus));
  emit runningChanged(false);
  emit statusChanged(displayStatus);
  emit infoUpdated();
}

void ShopExchangeController::changeSession(const QString& account, quint64 generation) {
  timeout_->stop();
  revokeAsyncRequests();
  const bool wasRunning = running_;
  running_ = false;
  requestAccount_.clear();
  requestSessionGeneration_ = 0;
  account_ = account;
  sessionGeneration_ = generation;
  freshness_.bindSession(account_, sessionGeneration_);
  // Eligibility for the return-player activities is read per session, so a new
  // session may query them again.
  notApplicableActivities_.clear();
  packet_ = {};
  hasPacket_ = false;
  materialCounts_.clear();
  hasMaterialCounts_ = false;
  cultivationMaterials_ = {};
  materialsOnly_ = false;
  anyUpdated_ = false;
  anyReadOnly_ = false;
  pendingCommands_.clear();
  fieldStates_.clear();
  observedAt_.clear();
  unverifiedPackets_ = {};
  activityObservations_ = {}; activityReads_.clear(); activeActivity_ = {};
  lastInboundSequence_ = 0;
  loadCache();
  if (wasRunning) emit runningChanged(false);
  emit infoUpdated();
  emit statusChanged(cacheLoading() ? QStringLiteral("正在异步读取当前账号的历史缓存")
                                    : QStringLiteral("当前账号尚无可读取的缓存"));
}

bool ShopExchangeController::cacheLoading() const {
  return (cacheStorage_ && cacheStorage_->loading()) || (cultivationMaterialStorage_ && cultivationMaterialStorage_->loading()) ||
      (activityStorage_ && activityStorage_->loading());
}
int ShopExchangeController::pendingStorageCount() const {
  return (cacheStorage_ ? cacheStorage_->pendingCount() : 0) +
         (cultivationMaterialStorage_ ? cultivationMaterialStorage_->pendingCount() : 0) + (activityStorage_ ? activityStorage_->pendingCount() : 0);
}
int ShopExchangeController::pendingWriteCount() const {
  return (cacheStorage_ ? cacheStorage_->pendingWriteCount() : 0) +
         (cultivationMaterialStorage_ ? cultivationMaterialStorage_->pendingWriteCount() : 0) + (activityStorage_ ? activityStorage_->pendingWriteCount() : 0);
}
QString ShopExchangeController::cacheStorageError() const {
  const auto primary = cacheStorage_ ? cacheStorage_->error() : QString{};
  const auto material = cultivationMaterialStorage_ ? cultivationMaterialStorage_->error() : QString{};
  const auto activity = activityStorage_ ? activityStorage_->error() : QString{};
  QStringList errors; for (const auto& error : {primary,material,activity}) if (!error.isEmpty()) errors.append(error);
  return errors.join(QStringLiteral("；"));
}

void ShopExchangeController::initializeCacheStorage() {
  cacheStorage_ = new ControllerCacheStorage(repository_->storageService(), QStringLiteral("shops.json"), 1024 * 1024, this);
  cacheStorage_->canWrite = [this] {
    return repository_ && repository_->sessionContext().canPersist() &&
        account_ == repository_->accountKey() && sessionGeneration_ == repository_->sessionGeneration();
  };
  cacheStorage_->snapshot = [this] { return cacheObject(); };
  cacheStorage_->loaded = [this](StorageStatus status, const QJsonObject& object, const QString& error) {
    QString message;
    if (status == StorageStatus::NotFound) message = QStringLiteral("当前账号尚无历史缓存");
    else if (status == StorageStatus::Loaded && applyCachedObject(object))
      message = QStringLiteral("已读取当前账号历史缓存，可手动刷新");
    else message = QStringLiteral("历史缓存读取失败，当前返回数据保留：%1").arg(error.isEmpty() ? QStringLiteral("账号、版本或字段无效") : error);
    emit infoUpdated();
    emit statusChanged(message);
  };
  cacheStorage_->changed = [this](const QString& account, quint64 epoch, quint64 revision, StorageStatus status, const QString& error) {
    emit persistenceChanged(account, epoch, revision, status, error);
    if (account != account_ || epoch != sessionGeneration_) return;
    if (status != StorageStatus::Queued && status != StorageStatus::Saved && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("缓存未保存，当前数据仍可查看：%1").arg(error));
  };
  cacheStorage_->stateChanged = [this] { emit cacheStateChanged(); };

  cultivationMaterialStorage_ = new ControllerCacheStorage(repository_->storageService(),
      QStringLiteral("cultivation-materials.json"), 1024 * 1024, this);
  cultivationMaterialStorage_->canWrite = [this] {
    const auto context = repository_ ? repository_->storageContext() : StorageContext{};
    return repository_ && repository_->isAuthenticated() && context && !context->isReadOnly() &&
        context->account() == account_ && account_ == repository_->accountKey() &&
        sessionGeneration_ == repository_->sessionGeneration();
  };
  cultivationMaterialStorage_->snapshot = [this] { return cultivationMaterialCacheObject(); };
  cultivationMaterialStorage_->loaded = [this](StorageStatus status, const QJsonObject& object, const QString& error) {
    if (status == StorageStatus::Loaded && !applyCultivationMaterialCache(object))
      emit statusChanged(QStringLiteral("材料缓存无效，已保留当前数量"));
    else if (status != StorageStatus::Loaded && status != StorageStatus::NotFound && status != StorageStatus::Cancelled)
      emit statusChanged(QStringLiteral("材料缓存读取失败，已保留当前数量：%1").arg(error));
    emit infoUpdated();
  };
  cultivationMaterialStorage_->changed = [this](const QString& account, quint64 epoch, quint64 revision,
                                               StorageStatus status, const QString& error) {
    emit persistenceChanged(account, epoch, revision, status, error);
    if (account == account_ && epoch == sessionGeneration_ && status != StorageStatus::Queued &&
        status != StorageStatus::Saved && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("材料数量未保存，当前数量仍可查看：%1").arg(error));
  };
  cultivationMaterialStorage_->stateChanged = [this] { emit cacheStateChanged(); };
  activityStorage_ = new ControllerCacheStorage(repository_->storageService(),QStringLiteral("activity-exchanges.json"),4 * 1024 * 1024,this);
  activityStorage_->canWrite = cultivationMaterialStorage_->canWrite;
  activityStorage_->snapshot = [this] { return QJsonObject{{QStringLiteral("schema"),1},{QStringLiteral("account"),account_},
      {QStringLiteral("sources"),activityObservations_}}; };
  activityStorage_->loaded = [this](StorageStatus status,const QJsonObject& object,const QString& error) {
    if (status == StorageStatus::Loaded && !applyActivityCache(object)) emit statusChanged(QStringLiteral("活动历史数据无效，保留当前观察"));
    else if (status != StorageStatus::Loaded && status != StorageStatus::NotFound && status != StorageStatus::Cancelled)
      emit statusChanged(QStringLiteral("活动缓存读取失败：%1").arg(error));
    emit infoUpdated();
  };
  activityStorage_->changed = [this](const QString& account,quint64 epoch,quint64 revision,StorageStatus status,const QString& error) {
    emit persistenceChanged(account,epoch,revision,status,error);
    if (account == account_ && epoch == sessionGeneration_ && status != StorageStatus::Queued && status != StorageStatus::Saved && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("活动数据未保存，当前观察仍可查看：%1").arg(error));
  };
  activityStorage_->stateChanged = [this] { emit cacheStateChanged(); };
}

void ShopExchangeController::loadCache() {
  if (!cacheStorage_) return;
  const bool readable = repository_ && repository_->isAuthenticated() && !account_.isEmpty();
  cacheStorage_->start(readable ? repository_->storageContext() : StorageContext{}, account_, sessionGeneration_);
  if (cultivationMaterialStorage_)
    cultivationMaterialStorage_->start(repository_ && !account_.isEmpty() ? repository_->storageContext() : StorageContext{},
                                       account_, sessionGeneration_);
  if (activityStorage_) activityStorage_->start(repository_ && !account_.isEmpty() ? repository_->storageContext() : StorageContext{},account_,sessionGeneration_);
}

void ShopExchangeController::saveCache() {
  if (!acceptingVerifiedPacket_ || !cacheStorage_) return;
  cacheStorage_->save();
}

bool ShopExchangeController::applyCachedObject(const QJsonObject& root) {
  qint64 schema = 0;
  if (!PacketContracts::checkedInteger(root.value(QStringLiteral("schema")), &schema, 1, 2) ||
      !root.value(QStringLiteral("account")).isString() || root.value(QStringLiteral("account")).toString() != account_ ||
      !root.value(QStringLiteral("packet")).isObject()) return false;
  const QJsonObject raw = root.value(QStringLiteral("packet")).toObject();
  QList<QJsonObject> roots{raw};
  for (const QString& wrapper : {QStringLiteral("data"), QStringLiteral("p"), QStringLiteral("params")}) {
    if (!raw.contains(wrapper)) continue;
    if (!raw.value(wrapper).isObject()) return false;
    roots.append(raw.value(wrapper).toObject());
  }
  QJsonObject candidate;
  for (const auto& shop : ShopExchangeCatalog::instance().protocolShops()) {
    const QString key = QStringLiteral("si%1").arg(shop.shopId);
    for (const QJsonObject& part : roots) {
      if (!part.contains(key)) continue;
      const QJsonValue value = part.value(key);
      if (!value.isObject() || !validShopGroup(value.toObject()) ||
          (candidate.contains(key) && candidate.value(key) != value)) return false;
      candidate.insert(key, value);
    }
  }
  QHash<QString, qint64> counts;
  if (schema >= 2 && root.contains(QStringLiteral("materialCounts"))) {
    if (!root.value(QStringLiteral("materialCounts")).isObject()) return false;
    const QJsonObject material = root.value(QStringLiteral("materialCounts")).toObject();
    for (auto value = material.begin(); value != material.end(); ++value) {
      const QStringList parts = value.key().split(QLatin1Char(':'));
      qint64 count = 0;
      if (parts.size() != 2 || !decimalKey(parts[0]) || !decimalKey(parts[1]) ||
          !PacketContracts::checkedInteger(value.value(), &count, 0)) return false;
      counts.insert(value.key(), count);
    }
  }
  // Fresh observations, including an explicitly empty group, take precedence.
  // Invalid/missing network groups may retain their historical display value,
  // but fieldStates_ remains invalid/missing rather than becoming fresh.
  for (auto group = candidate.begin(); group != candidate.end(); ++group)
    if (!observedAt_.contains(group.key())) packet_.insert(group.key(), group.value());
  for (auto count = counts.begin(); count != counts.end(); ++count)
    if (!observedAt_.contains(QStringLiteral("material:") + count.key().section(QLatin1Char(':'), 0, 0)))
      materialCounts_.insert(count.key(), count.value());
  return true;
}

QJsonObject ShopExchangeController::cacheObject() const {
  QJsonObject counts;
  for (auto value = materialCounts_.cbegin(); value != materialCounts_.cend(); ++value)
    counts.insert(value.key(), QString::number(value.value()));
  return {{QStringLiteral("schema"), 2}, {QStringLiteral("account"), account_},
          {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
          {QStringLiteral("packet"), packet_}, {QStringLiteral("hasMaterialCounts"), hasMaterialCounts_},
          {QStringLiteral("materialCounts"), counts}};
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

bool ShopExchangeController::acceptShopPacket(const QJsonObject& packet,
                                               QStringList* warnings, QJsonObject* readOnly) {
  QList<QJsonObject> roots{packet};
  for (const QString& wrapper : {QStringLiteral("data"), QStringLiteral("p"),
                                 QStringLiteral("params")}) {
    if (!packet.contains(wrapper)) continue;
    if (!packet.value(wrapper).isObject()) {
      warnings->append(QStringLiteral("商店%1包装类型错误").arg(wrapper));
      return false;
    }
    roots.append(packet.value(wrapper).toObject());
  }
  bool updated = false;
  bool allValid = true;
  QJsonObject candidate = readOnly ? *readOnly : packet_;
  for (const auto& shop : ShopExchangeCatalog::instance().protocolShops()) {
    const QString key = QStringLiteral("si%1").arg(shop.shopId);
    QJsonValue value;
    bool found = false;
    bool valid = true;
    for (const auto& root : roots) {
      if (!root.contains(key)) continue;
      if (found && value != root.value(key)) valid = false;
      value = root.value(key);
      found = true;
    }
    if (!found) {
      if (!readOnly) fieldStates_.insert(key, PacketFieldState::Missing);
      warnings->append(QStringLiteral("商店%1缺失").arg(shop.shopId));
      allValid = false;
      continue;
    }
    if (!valid || !value.isObject() || !validShopGroup(value.toObject())) {
      if (!readOnly) fieldStates_.insert(key, PacketFieldState::Invalid);
      warnings->append(QStringLiteral("商店%1数据无效").arg(shop.shopId));
      allValid = false;
      continue;
    }
    candidate.insert(key, value);
    if (!readOnly) {
      fieldStates_.insert(key, value.toObject().isEmpty() ? PacketFieldState::Empty
                                                        : PacketFieldState::Value);
      observeGroup(key);
    }
    updated = true;
  }
  if (updated) {
    // Flatten validated groups so stale wrappers cannot shadow fresh values.
    candidate.remove(QStringLiteral("data"));
    candidate.remove(QStringLiteral("p"));
    candidate.remove(QStringLiteral("params"));
    candidate.insert(QStringLiteral("_cmd"), ShopExchangeCatalog::instance().getInfoCommand());
    if (readOnly) *readOnly = candidate;
    else packet_ = candidate;
  }
  if (!readOnly) hasPacket_ = updated && allValid;
  return updated;
}
