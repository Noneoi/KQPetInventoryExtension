#include "routine_overview_controller.h"

#include "diagnostics/diagnostic_logger.h"
#include "application/common/controller_cache_storage.h"
#include "application/catalog/catalog_io_service.h"
#include "application/pet/pet_repository.h"
#include "application/catalog/routine_overview_catalog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

const QString kDailyCommand = QStringLiteral("1008_20170623_dt_0");
const QString kRedPointCommand = QStringLiteral("1037_0");
const QString kStarWheelCommand = QStringLiteral("1008_20220603_swa_0_0");
const QString kArenaCommand = QStringLiteral("16_24_A");
const QString kColorfulTreeCommand = QStringLiteral("1008_20190531_gbt_1");
const QString kSourceBeastGateCommand = QStringLiteral("2_36_1");
const QString kNationalCompetitionCommand = QStringLiteral("110_123_0");
const QString kNewFarmCommand = QStringLiteral("1008_20260522_nf_0");

bool counter(const QJsonValue& value, int* result) {
  qint64 parsed = 0;
  if (!PacketContracts::checkedInteger(value, &parsed, 0,
                                        std::numeric_limits<int>::max())) return false;
  *result = static_cast<int>(parsed);
  return true;
}

bool successfulReply(const QJsonObject& packet) {
  qint64 result = 0;
  return !packet.contains(QStringLiteral("r")) ||
         (PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &result) &&
          result == 1);
}

bool normalizeCounters(const QJsonObject& packet, const QStringList& fields,
                        QJsonObject* candidate) {
  *candidate = packet;
  for (const QString& field : fields) {
    int value = 0;
    if (!counter(packet.value(field), &value)) return false;
    candidate->insert(field, value);
  }
  return true;
}

QStringList dailyFields() {
  return {QStringLiteral("ti"), QStringLiteral("wti"), QStringLiteral("av"),
          QStringLiteral("wav"), QStringLiteral("bi"), QStringLiteral("wbi")};
}

QStringList opportunityFields(const QString& command) {
  if (command == kStarWheelCommand) return {QStringLiteral("ti"), QStringLiteral("wgt")};
  if (command == kColorfulTreeCommand) return {QStringLiteral("ti")};
  if (command == kSourceBeastGateCommand) return {QStringLiteral("t")};
  if (command == kNationalCompetitionCommand)
    return {QStringLiteral("rwwt"), QStringLiteral("rdt"), QStringLiteral("rdb"), QStringLiteral("wwt")};
  if (command == kNewFarmCommand) return {QStringLiteral("pt"), QStringLiteral("rft")};
  return {};
}

bool parseRedPoints(const QJsonValue& value, QSet<int>* points) {
  if (!value.isString()) return false;
  const QString raw = value.toString();
  if (raw.isEmpty()) return true;
  for (const QString& token : raw.split(QLatin1Char('#'), Qt::KeepEmptyParts)) {
    int id = 0;
    if (!counter(token, &id) || id <= 0 || QString::number(id) != token) return false;
    points->insert(id);
  }
  return true;
}

}  // namespace

RoutineOverviewController::RoutineOverviewController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  timeout_ = new QTimer(this);
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this]() {
    if (asyncSender_) { pollAsyncRequests(); return; }
    DiagnosticLogger::warning(
        QStringLiteral("timeout"),
        QStringLiteral("routine refresh timed out pending_count=%1")
            .arg(pendingCommands_.size()));
    for (const QString& command : std::as_const(pendingCommands_))
      requestWarnings_.append(QStringLiteral("%1超时").arg(requestLabels_.value(command, command)));
    pendingCommands_.clear();
    finish(anyUpdated_,
           QStringLiteral("%1；未返回部分保留旧缓存")
               .arg(requestWarnings_.join(QStringLiteral("、"))));
  });
  if (repository_) {
    initializeCacheStorage();
    account_ = repository_->accountKey();
    sessionGeneration_ = repository_->sessionGeneration();
    freshness_.bindSession(account_, sessionGeneration_);
    connect(repository_, &PetRepository::accountSessionChanged, this,
            &RoutineOverviewController::changeSession);
    connect(repository_, &PetRepository::sessionTrustChanged, this,
        [this](SessionConnectionState, const QString&) { checkFreshness(); });
    loadCache();
  }
}

void RoutineOverviewController::setSender(Sender sender) { sender_ = std::move(sender); }

void RoutineOverviewController::setCatalogIoService(CatalogIoService* service) {
  if (catalogIo_) disconnect(catalogIo_, nullptr, this, nullptr);
  catalogIo_ = service;
  if (!service) return;
  connect(service, &CatalogIoService::catalogUpdated, this, [this](CatalogKind kind, quint64) {
    if (kind == CatalogKind::Routine) emit catalogUpdated();
  });
  connect(service, &CatalogIoService::finished, this,
      [this](quint64, CatalogKind kind, StorageStatus status, const QString& error) {
    if (kind != CatalogKind::Routine || status == StorageStatus::Cancelled || status == StorageStatus::NotFound) return;
    emit statusChanged(status == StorageStatus::Saved || status == StorageStatus::Loaded
        ? QStringLiteral("官方目录已更新")
        : QStringLiteral("目录未更新，继续使用上一版本：%1").arg(error));
  });
}


RoutineOverviewController::~RoutineOverviewController() { revokeAsyncRequests(); }

void RoutineOverviewController::setObservationClock(ObservationClock clock) {
  freshness_.setClock(std::move(clock));
  if (!checkFreshness()) publishFreshness();
}

QString periodForGroup(const QString& group) {
  if (group == QStringLiteral("av") || group == QStringLiteral("bi") || group == QStringLiteral("ti") || group == QStringLiteral("wdti"))
    return QStringLiteral("daily");
  if (group == QStringLiteral("wav") || group == QStringLiteral("wbi") || group == QStringLiteral("wti"))
    return QStringLiteral("weekly");
  return QStringLiteral("activity");
}
void RoutineOverviewController::publishFreshness() {
  const QPointer<RoutineOverviewController> alive(this);
  emit freshnessChanged();
  if (alive) emit dataUpdated();
}
void RoutineOverviewController::observeGroup(const QString& group) {
  freshness_.observe(group, lastInboundSequence_, capturedMonotonicMs_);
  observedAt_.insert(group, freshness_.observedAt(group));
}
bool RoutineOverviewController::checkFreshness() {
  const quint64 before = freshness_.revision();
  if (!repository_ || !repository_->sessionContext().canPersist())
    freshness_.invalidateObservations(QStringLiteral("会话来源未确认或已失效"));
  freshness_.check();
  if (before == freshness_.revision()) return false;
  publishFreshness();
  return true;
}
bool RoutineOverviewController::acceptPeriodValidityEvidence(const TrustedObservationValidity& evidence, QString* error) {
  const QPointer<RoutineOverviewController> alive(this);
  checkFreshness();
  if (!alive) return false;
  const QString expectedPeriod = periodForGroup(evidence.group);
  if (evidence.periodKey != expectedPeriod || !observedAt_.contains(evidence.group) ||
      (fieldState(evidence.group) != PacketFieldState::Empty && fieldState(evidence.group) != PacketFieldState::Value)) {
    if (error) *error = QStringLiteral("周期证据不对应有效日常/活动观察与已登记的周期键");
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
QHash<QString, ObservationValidity> RoutineOverviewController::periodValiditySnapshot() const {
  QHash<QString, ObservationValidity> result;
  for (auto observed = observedAt_.begin(); observed != observedAt_.end(); ++observed) {
    const QString group = observed.key();
    const QString period = periodForGroup(group);
    auto value = freshness_.status(group, period);
    if (!repository_ || !repository_->sessionContext().canPersist() ||
        (fieldState(group) != PacketFieldState::Empty && fieldState(group) != PacketFieldState::Value)) {
      value.state = ObservationValidityState::Invalidated;
      value.reason = QStringLiteral("分组或来源未更新；保留只读旧观察");
    }
    result.insert(group + QLatin1Char(':') + period, value);
  }
  return result;
}
QString RoutineOverviewController::freshnessSummary() const {
  const auto values = periodValiditySnapshot();
  bool known = false, stale = false;
  for (const auto& value : values) {
    known |= value.current(); stale |= value.state == ObservationValidityState::Invalidated;
  }
  if (stale) return QStringLiteral("部分日常/活动周期已失效，保留只读观察");
  return known ? QStringLiteral("已核验部分周期，其余保持待确认") : QStringLiteral("日常/活动周期未核验；当前数值仅为只读观察");
}

bool RoutineOverviewController::queueRequest(const QString& extension, const QString& command,
                                              const QString& parameters) {
  if (!asyncSender_ || !running_ || !repository_ || requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != repository_->sessionGeneration()) return false;
  return requests_.queue(asyncSender_, requestAccount_, requestSessionGeneration_,
                         repository_->sessionContext().source, extension, command, parameters);
}

void RoutineOverviewController::revokeAsyncRequests() { requests_.revokeAll(); }

void RoutineOverviewController::handleSendReceipt(const SendReceipt& receipt) {
  const auto intent = requests_.takeIntent(receipt);
  if (!intent) return;
  if (!running_ || !pendingCommands_.contains(receipt.command) || !repository_ ||
      receipt.account != repository_->accountKey() || receipt.sessionEpoch != repository_->sessionGeneration()) return;
  if (receipt.outcome == SubmissionOutcome::DefinitelyNotSubmitted || receipt.dispatchedAtMs < 0) {
    completeRequest(receipt.command, false, QStringLiteral("%1未提交：%2").arg(receipt.command, receipt.error));
    return;
  }
  if (!requests_.recordDispatch(receipt.command, receipt.dispatchedAtMs, intent->ticket.responseTimeoutMs))
    completeRequest(receipt.command, false, QStringLiteral("%1实际发送后的响应已超时").arg(receipt.command));
}

void RoutineOverviewController::pollAsyncRequests() {
  const qint64 now = transportMonotonicMs();
  for (const auto& receipt : requests_.pollReceipts(now)) handleSendReceipt(receipt);
  for (const QString& command : requests_.expiredCommands(now))
    completeRequest(command, false, QStringLiteral("%1响应超时").arg(command));
  if (running_) timeout_->start(10);
}

QJsonObject RoutineOverviewController::opportunityPackets() const {
  QJsonObject current;
  for (auto packet = opportunityPackets_.begin(); packet != opportunityPackets_.end(); ++packet) {
    if (packet.key() == kArenaCommand) {
      QJsonObject arena;
      for (const QString& group : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
        if (fieldState(kArenaCommand + QLatin1Char(':') + group) == PacketFieldState::Value)
          arena.insert(group, packet.value().toObject().value(group));
      }
      if (!arena.isEmpty()) current.insert(packet.key(), arena);
    } else if (fieldState(packet.key()) == PacketFieldState::Value)
      current.insert(packet.key(), packet.value());
  }
  return current;
}

bool RoutineOverviewController::requestRefresh() {
  if (running_) return false;
  if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty()) {
    emit statusChanged(QStringLiteral("请先登录并识别账号，再手动刷新任务与活动状态"));
    return false;
  }
  if (!sender_ && !asyncSender_) {
    emit statusChanged(QStringLiteral("原版发送入口尚未就绪"));
    return false;
  }
  if (catalogIo_) catalogIo_->requestOfficialUpdate(CatalogKind::Routine);

  requestAccount_ = account_;
  requestSessionGeneration_ = sessionGeneration_;
  anyUpdated_ = false;
  anyReadOnly_ = false;
  pendingCommands_.clear();
  requestLabels_.clear();
  requestWarnings_.clear();
  revokeAsyncRequests();
  hasDailyPacket_ = false;
  hasRedPointPacket_ = false;
  for (const QString& field : dailyFields())
    fieldStates_.insert(field, PacketFieldState::Missing);
  fieldStates_.insert(QStringLiteral("rs"), PacketFieldState::Missing);
  running_ = true;
  DiagnosticLogger::info(QStringLiteral("routine"),
                         QStringLiteral("refresh started session_generation=%1")
                             .arg(requestSessionGeneration_));
  emit runningChanged(true);
  emit statusChanged(QStringLiteral("正在手动查询任务、活动红点和玩法剩余次数……"));
  // Register the whole batch before invoking any sender. A synchronous fake
  // (or future adapter) response must not finish the batch after its first item.
  pendingCommands_ = {kDailyCommand, kRedPointCommand, kStarWheelCommand,
                      kColorfulTreeCommand, kSourceBeastGateCommand,
                      kNationalCompetitionCommand, kNewFarmCommand};
  for (const QString& command : std::as_const(pendingCommands_))
    fieldStates_.insert(command, PacketFieldState::Missing);
  emit dataUpdated();
  startingRequests_ = true;
  const auto sendRequest = [this](const QString& service, const QString& command,
                                  const QString& parameters, const QString& label) {
    requestLabels_.insert(command, label);
    if (!asyncSender_) requests_.markDispatchedNow(command);
    if (asyncSender_ ? queueRequest(service, command, parameters) : sender_(service, command, parameters)) return;
    requests_.forgetDispatch(command);
    pendingCommands_.remove(command);
    requestWarnings_.append(QStringLiteral("%1请求发送失败").arg(label));
  };
  sendRequest(QStringLiteral("TimelinessActExtension"), kDailyCommand,
              QStringLiteral("null"), QStringLiteral("日常/周常"));
  sendRequest(QStringLiteral("null"), kRedPointCommand,
              QStringLiteral("{\"ids\":\"lights\"}"), QStringLiteral("活动红点"));
  sendRequest(QStringLiteral("TimelinessActExtension"), kStarWheelCommand,
              QStringLiteral("null"), QStringLiteral("星轮探险次数"));
  sendRequest(QStringLiteral("TimelinessActExtension"), kColorfulTreeCommand,
              QStringLiteral("null"), QStringLiteral("缤纷树次数"));
  sendRequest(QStringLiteral("PJXExtension"), kSourceBeastGateCommand,
              QStringLiteral("null"), QStringLiteral("源兽之门次数"));
  sendRequest(QStringLiteral("XiaoMoEvolveExtension"), kNationalCompetitionCommand,
              QStringLiteral("null"), QStringLiteral("全民斗技次数"));
  sendRequest(QStringLiteral("TimelinessActExtension"), kNewFarmCommand,
              QStringLiteral("{\"un\":-1}"), QStringLiteral("新版农场次数"));
  startingRequests_ = false;
  if (pendingCommands_.isEmpty()) {
    finish(anyUpdated_, requestWarnings_.isEmpty()
         ? QStringLiteral("已更新任务与活动状态")
         : requestWarnings_.join(QStringLiteral("；")));
    return anyUpdated_;
  }
  timeout_->start(asyncSender_ ? 10 : 10000);
  return true;
}

void RoutineOverviewController::handlePacket(const QString& method, const QString& payload) {
  QJsonObject packet;
  if (!PacketContracts::decodeObject(method, payload, &packet)) return;
  handleDecodedEnvelope({}, packet);
}

void RoutineOverviewController::handleDecodedEnvelope(const InboundEnvelope& envelope,
                                                        const QJsonObject& packet) {
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  const QSet<QString> accepted{kDailyCommand, kRedPointCommand, kStarWheelCommand,
      kArenaCommand, kColorfulTreeCommand, kSourceBeastGateCommand,
      kNationalCompetitionCommand, kNewFarmCommand};
  if (!accepted.contains(command)) return;
  if (!repository_ || !repository_->sessionContext().accepts(envelope)) {
    if (!matchesReadOnlyRequest(envelope, command)) return;
    lastInboundSequence_ = envelope.receiveSequence;
    QStringList warnings;
    QJsonObject observation;
    bool updated = false;
    if (!successfulReply(packet))
      warnings.append(QStringLiteral("%1被服务器拒绝或结果类型错误").arg(requestLabels_.value(command, command)));
    else if (command == kDailyCommand)
      updated = acceptDailyPacket(packet, &warnings, &observation);
    else if (command == kRedPointCommand) {
      QSet<int> points;
      updated = parseRedPoints(packet.value(QStringLiteral("rs")), &points);
      if (updated) observation.insert(QStringLiteral("rs"), packet.value(QStringLiteral("rs")));
      else warnings.append(QStringLiteral("活动红点rs缺失或类型/编号无效"));
    } else {
      const auto fields = opportunityFields(command);
      updated = !fields.isEmpty() && normalizeCounters(packet, fields, &observation);
      if (updated && command == kNationalCompetitionCommand &&
          observation.value(QStringLiteral("rdb")).toInt() > std::numeric_limits<int>::max() - 40)
        updated = false;
      if (!updated) warnings.append(QStringLiteral("%1数据不完整").arg(requestLabels_.value(command, command)));
    }
    if (updated) {
      QJsonObject retained = unverifiedPackets_.value(command).toObject();
      for (auto field = observation.begin(); field != observation.end(); ++field)
        retained.insert(field.key(), field.value());
      unverifiedPackets_.insert(command, retained);
      anyReadOnly_ = true;
    }
    completeRequest(command, updated, warnings.join(QStringLiteral("、")));
    return;
  }
  if (envelope.receiveSequence == 0 || envelope.receiveSequence <= lastInboundSequence_) return;
  lastInboundSequence_ = envelope.receiveSequence;
  capturedMonotonicMs_ = envelope.receivedMonotonicMs;
  unverifiedPackets_.remove(command);
  acceptingVerifiedPacket_ = true;
  handleVerifiedPacket(packet);
  acceptingVerifiedPacket_ = false;
}

bool RoutineOverviewController::matchesReadOnlyRequest(const InboundEnvelope& envelope,
                                                       const QString& command) const {
  if (!repository_ || !running_ || !pendingCommands_.contains(command) ||
      envelope.source.verified() || repository_->sessionContext().source.verified() ||
      repository_->sessionContext().state != SessionConnectionState::Uncertain ||
      !repository_->isAuthenticated() || requestAccount_ != repository_->accountKey() ||
      requestAccount_ != account_ || requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration() ||
      (envelope.capturedSessionEpoch != 0 && envelope.capturedSessionEpoch != requestSessionGeneration_) ||
      envelope.receiveSequence == 0 || envelope.receiveSequence <= lastInboundSequence_) return false;
  const qint64 dispatched = requests_.dispatchedAt(command);
  // Unknown production host epochs stay zero. Match the local request only;
  // no account/source/order authority is inferred from this observation.
  return dispatched >= 0 && envelope.receivedMonotonicMs >= dispatched;
}

void RoutineOverviewController::handleVerifiedPacket(const QJsonObject& packet) {
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  // 16_24_A updates the game's ArenaV3 challenge cooldown as a side effect.
  // Never request it here.  When the game itself opens ArenaV3, keep only the
  // returned challenge counters so the overview can still show exact values.
  if (command == kArenaCommand) {
    if (!repository_ || !repository_->isAuthenticated() || account_.isEmpty() ||
        account_ != repository_->accountKey() ||
        sessionGeneration_ != repository_->sessionGeneration())
      return;
    if (!successfulReply(packet)) {
      fieldStates_.insert(command + QStringLiteral(":zao1"), PacketFieldState::Invalid);
      fieldStates_.insert(command + QStringLiteral(":zao2"), PacketFieldState::Invalid);
      emit dataUpdated();
      return;
    }
    QJsonObject candidate = opportunityPackets_.value(command).toObject();
    bool updated = false;
    for (const QString& group : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
      const QString stateKey = command + QLatin1Char(':') + group;
      const QJsonValue value = packet.value(group);
      QJsonObject counters;
      if (!value.isObject() || !normalizeCounters(value.toObject(),
            {QStringLiteral("ct"), QStringLiteral("bct")}, &counters) ||
          counters.value(QStringLiteral("bct")).toInt() > std::numeric_limits<int>::max() - 8) {
        fieldStates_.insert(stateKey, value.isUndefined() ? PacketFieldState::Missing
                                                         : PacketFieldState::Invalid);
        continue;
      }
      // Capture only the documented counters; cooldown/ranking data is not
      // part of this read-only opportunity contract.
      candidate.insert(group, QJsonObject{{QStringLiteral("ct"), counters.value(QStringLiteral("ct"))},
                                          {QStringLiteral("bct"), counters.value(QStringLiteral("bct"))}});
      fieldStates_.insert(stateKey, PacketFieldState::Value);
      observeGroup(stateKey);
      updated = true;
    }
    if (updated) { opportunityPackets_.insert(command, candidate); saveCache(); }
    emit dataUpdated();
    return;
  }
  if (!running_) return;
  if (!pendingCommands_.contains(command)) return;
  if (!repository_ || !repository_->isAuthenticated() || requestAccount_ != account_ ||
      requestAccount_ != repository_->accountKey() ||
      requestSessionGeneration_ != sessionGeneration_ ||
      requestSessionGeneration_ != repository_->sessionGeneration()) {
    finish(false, QStringLiteral("账号已切换，已忽略旧账号的任务/活动响应"));
    return;
  }
  if (!successfulReply(packet)) {
    fieldStates_.insert(command, PacketFieldState::Invalid);
    completeRequest(command, false,
                    QStringLiteral("%1被服务器拒绝").arg(requestLabels_.value(command)));
    return;
  }
  if (command == kDailyCommand) {
    QStringList warnings;
    const bool updated = acceptDailyPacket(packet, &warnings);
    fieldStates_.insert(command, hasDailyPacket_ ? PacketFieldState::Value
                                               : PacketFieldState::Invalid);
    completeRequest(command, updated, warnings.join(QStringLiteral("、")));
  } else if (command == kRedPointCommand) {
    QSet<int> points;
    const QJsonValue value = packet.value(QStringLiteral("rs"));
    const bool valid = parseRedPoints(value, &points);
    if (!valid) {
      fieldStates_.insert(QStringLiteral("rs"), value.isUndefined() ? PacketFieldState::Missing
                                                                    : PacketFieldState::Invalid);
      completeRequest(command, false, QStringLiteral("活动红点rs缺失或类型/编号无效"));
      return;
    }
    activeRedPoints_ = points;
    hasRedPointPacket_ = true;
    fieldStates_.insert(QStringLiteral("rs"), points.isEmpty() ? PacketFieldState::Empty
                                                              : PacketFieldState::Value);
    observeGroup(QStringLiteral("rs"));
    completeRequest(command, true);
  } else {
    const QStringList fields = opportunityFields(command);
    QJsonObject candidate;
    bool valid = !fields.isEmpty() && normalizeCounters(packet, fields, &candidate);
    if (valid && command == kNationalCompetitionCommand &&
        candidate.value(QStringLiteral("rdb")).toInt() > std::numeric_limits<int>::max() - 40)
      valid = false;
    if (!valid) {
      fieldStates_.insert(command, PacketFieldState::Invalid);
      completeRequest(command, false,
                      QStringLiteral("%1数据不完整").arg(requestLabels_.value(command)));
      return;
    }
    opportunityPackets_.insert(command, candidate);
    fieldStates_.insert(command, PacketFieldState::Value);
    observeGroup(command);
    completeRequest(command, true);
  }
}

void RoutineOverviewController::completeRequest(const QString& command, bool updated,
                                                const QString& warning) {
  if (!pendingCommands_.remove(command)) return;
  requests_.forget(command);
  if (updated) {
    anyUpdated_ = true;
    saveCache();
  }
  if (!warning.isEmpty()) requestWarnings_.append(warning);
  if (pendingCommands_.isEmpty() && !startingRequests_) {
    const QString status = requestWarnings_.isEmpty()
                               ? QStringLiteral("已更新任务、活动红点与已接入玩法的真实剩余次数")
                               : QStringLiteral("%1；%2，该部分保留旧缓存")
                                     .arg(anyUpdated_ ? QStringLiteral("已更新可用数据")
                                                     : QStringLiteral("未更新数据"),
                                          requestWarnings_.join(QStringLiteral("、")));
    finish(anyUpdated_, status);
  } else {
    emit dataUpdated();
  }
}

bool RoutineOverviewController::acceptDailyPacket(const QJsonObject& packet,
                                                   QStringList* warnings, QJsonObject* readOnly) {
  QJsonObject candidate = readOnly ? *readOnly : dailyPacket_;
  bool updated = false;
  bool complete = true;
  QStringList fields = dailyFields();
  if (packet.contains(QStringLiteral("wdti"))) fields.append(QStringLiteral("wdti"));
  for (const QString& field : fields) {
    const QJsonValue value = packet.value(field);
    if (value.isUndefined()) {
      if (!readOnly) fieldStates_.insert(field, PacketFieldState::Missing);
      warnings->append(QStringLiteral("%1缺失").arg(field));
      complete = false;
      continue;
    }
    QJsonValue normalized;
    bool valid = false;
    if (field == QStringLiteral("av") || field == QStringLiteral("wav")) {
      int count = 0;
      valid = counter(value, &count);
      normalized = count;
    } else if (value.isArray()) {
      QJsonArray array;
      valid = true;
      const bool booleanArray = field == QStringLiteral("bi") || field == QStringLiteral("wbi");
      for (const QJsonValue& entry : value.toArray()) {
        int count = 0;
        if (booleanArray) {
          if (!entry.isBool()) { valid = false; break; }
          array.append(entry);
        } else {
          if (!counter(entry, &count)) { valid = false; break; }
          array.append(count);
        }
      }
      normalized = array;
    }
    if (!valid) {
      if (!readOnly) fieldStates_.insert(field, PacketFieldState::Invalid);
      warnings->append(QStringLiteral("%1类型或数值无效").arg(field));
      complete = false;
      continue;
    }
    candidate.insert(field, normalized);
    if (!readOnly) {
      fieldStates_.insert(field, normalized.isArray() && normalized.toArray().isEmpty()
                                    ? PacketFieldState::Empty : PacketFieldState::Value);
      observeGroup(field);
    }
    updated = true;
  }
  if (updated) {
    candidate.insert(QStringLiteral("_cmd"), kDailyCommand);
    if (readOnly) *readOnly = candidate;
    else dailyPacket_ = candidate;
  }
  if (!readOnly) hasDailyPacket_ = complete && updated;
  return updated;
}

void RoutineOverviewController::finish(bool publish, const QString& status) {
  const QString displayStatus = anyReadOnly_
      ? QStringLiteral("已接收只读任务/活动数据；来源未确认，未写入账号缓存") +
          (requestWarnings_.isEmpty() ? QString{} : QStringLiteral("；%1，未更新部分保留旧观察")
              .arg(requestWarnings_.join(QStringLiteral("、"))))
      : status;
  timeout_->stop();
  revokeAsyncRequests();
  pendingCommands_.clear();
  running_ = false;
  if (publish)
    DiagnosticLogger::info(QStringLiteral("routine"),
                           QStringLiteral("refresh completed: %1").arg(displayStatus));
  else
    DiagnosticLogger::error(QStringLiteral("routine"),
                            QStringLiteral("refresh ended without update: %1").arg(displayStatus));
  emit runningChanged(false);
  emit statusChanged(displayStatus);
  emit dataUpdated();
}

void RoutineOverviewController::changeSession(const QString& account, quint64 generation) {
  timeout_->stop();
  revokeAsyncRequests();
  const bool wasRunning = running_;
  running_ = false;
  requestAccount_.clear();
  requestSessionGeneration_ = 0;
  account_ = account;
  sessionGeneration_ = generation;
  freshness_.bindSession(account_, sessionGeneration_);
  dailyPacket_ = {};
  activeRedPoints_.clear();
  opportunityPackets_ = {};
  fieldStates_.clear();
  observedAt_.clear();
  unverifiedPackets_ = {};
  anyReadOnly_ = false;
  lastInboundSequence_ = 0;
  hasDailyPacket_ = false;
  hasRedPointPacket_ = false;
  loadCache();
  if (wasRunning) emit runningChanged(false);
  emit dataUpdated();
  emit statusChanged(cacheLoading() ? QStringLiteral("正在异步读取当前账号的历史缓存")
                                    : QStringLiteral("当前账号尚无可读取的缓存"));
}

bool RoutineOverviewController::cacheLoading() const { return cacheStorage_ && cacheStorage_->loading(); }
int RoutineOverviewController::pendingStorageCount() const { return cacheStorage_ ? cacheStorage_->pendingCount() : 0; }
int RoutineOverviewController::pendingWriteCount() const { return cacheStorage_ ? cacheStorage_->pendingWriteCount() : 0; }
QString RoutineOverviewController::cacheStorageError() const { return cacheStorage_ ? cacheStorage_->error() : QString{}; }

void RoutineOverviewController::initializeCacheStorage() {
  cacheStorage_ = new ControllerCacheStorage(repository_->storageService(), QStringLiteral("routines.json"), 1024 * 1024, this);
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
    emit dataUpdated();
    emit statusChanged(message);
  };
  cacheStorage_->changed = [this](const QString& account, quint64 epoch, quint64 revision, StorageStatus status, const QString& error) {
    emit persistenceChanged(account, epoch, revision, status, error);
    if (account != account_ || epoch != sessionGeneration_) return;
    if (status != StorageStatus::Queued && status != StorageStatus::Saved && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("缓存未保存，当前数据仍可查看：%1").arg(error));
  };
  cacheStorage_->stateChanged = [this] { emit cacheStateChanged(); };
}

void RoutineOverviewController::loadCache() {
  if (!cacheStorage_) return;
  const bool readable = repository_ && repository_->isAuthenticated() && !account_.isEmpty();
  cacheStorage_->start(readable ? repository_->storageContext() : StorageContext{}, account_, sessionGeneration_);
}

void RoutineOverviewController::saveCache() {
  if (!acceptingVerifiedPacket_ || !cacheStorage_) return;
  cacheStorage_->save();
}

bool RoutineOverviewController::applyCachedObject(const QJsonObject& root) {
  qint64 schema = 0;
  if (!PacketContracts::checkedInteger(root.value(QStringLiteral("schema")), &schema, 1, 4) ||
      !root.value(QStringLiteral("account")).isString() || root.value(QStringLiteral("account")).toString() != account_) return false;
  QJsonObject daily;
  if (root.contains(QStringLiteral("dailyPacket"))) {
    if (!root.value(QStringLiteral("dailyPacket")).isObject()) return false;
    const QJsonObject raw = root.value(QStringLiteral("dailyPacket")).toObject();
    QStringList fields = dailyFields(); fields.append(QStringLiteral("wdti"));
    for (const QString& field : fields) {
      if (!raw.contains(field)) continue;
      const QJsonValue value = raw.value(field);
      if (field == QStringLiteral("av") || field == QStringLiteral("wav")) {
        int count = 0;
        if (!counter(value, &count)) return false;
        daily.insert(field, count);
      } else {
        if (!value.isArray()) return false;
        QJsonArray array;
        const bool flags = field == QStringLiteral("bi") || field == QStringLiteral("wbi");
        for (const auto& entry : value.toArray()) {
          int count = 0;
          if (flags) { if (!entry.isBool()) return false; array.append(entry); }
          else { if (!counter(entry, &count)) return false; array.append(count); }
        }
        daily.insert(field, array);
      }
    }
  }
  QSet<int> points;
  if (root.contains(QStringLiteral("redPoints"))) {
    if (!root.value(QStringLiteral("redPoints")).isArray()) return false;
    for (const auto& entry : root.value(QStringLiteral("redPoints")).toArray()) {
      int point = 0;
      if (!counter(entry, &point) || point <= 0 || points.contains(point)) return false;
      points.insert(point);
    }
  }
  QJsonObject rawOpportunities;
  if (schema >= 3 && root.contains(QStringLiteral("opportunityPackets"))) {
    if (!root.value(QStringLiteral("opportunityPackets")).isObject()) return false;
    rawOpportunities = root.value(QStringLiteral("opportunityPackets")).toObject();
  } else if (root.contains(QStringLiteral("opportunityPacket"))) {
    if (!root.value(QStringLiteral("opportunityPacket")).isObject() ||
        (root.contains(QStringLiteral("hasOpportunityPacket")) && !root.value(QStringLiteral("hasOpportunityPacket")).isBool())) return false;
    if (root.value(QStringLiteral("hasOpportunityPacket")).toBool())
      rawOpportunities.insert(kStarWheelCommand, root.value(QStringLiteral("opportunityPacket")));
  }
  const QHash<QString, QStringList> required{{kStarWheelCommand, {QStringLiteral("ti"), QStringLiteral("wgt")}},
      {kColorfulTreeCommand, {QStringLiteral("ti")}}, {kSourceBeastGateCommand, {QStringLiteral("t")}},
      {kNationalCompetitionCommand, {QStringLiteral("rwwt"), QStringLiteral("rdt"), QStringLiteral("rdb"), QStringLiteral("wwt")}},
      {kNewFarmCommand, {QStringLiteral("pt"), QStringLiteral("rft")}}};
  QJsonObject opportunities;
  for (auto entry = rawOpportunities.begin(); entry != rawOpportunities.end(); ++entry) {
    const QString command = entry.key();
    if (!required.contains(command) && command != kArenaCommand) continue;
    if (command == kArenaCommand && schema < 4) continue; // legacy actively requested Arena is not an accepted observation
    if (!entry.value().isObject()) return false;
    QJsonObject candidate;
    if (command == kArenaCommand) {
      const QJsonObject raw = entry.value().toObject();
      for (const QString& group : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
        if (!raw.contains(group)) continue;
        QJsonObject counters;
        if (!raw.value(group).isObject() || !normalizeCounters(raw.value(group).toObject(),
              {QStringLiteral("ct"), QStringLiteral("bct")}, &counters) ||
            counters.value(QStringLiteral("bct")).toInt() > std::numeric_limits<int>::max() - 8) return false;
        candidate.insert(group, counters);
      }
    } else if (!normalizeCounters(entry.value().toObject(), required.value(command), &candidate) ||
               (command == kNationalCompetitionCommand && candidate.value(QStringLiteral("rdb")).toInt() > std::numeric_limits<int>::max() - 40)) return false;
    opportunities.insert(command, candidate);
  }
  for (auto field = daily.begin(); field != daily.end(); ++field)
    if (!observedAt_.contains(field.key())) dailyPacket_.insert(field.key(), field.value());
  if (!observedAt_.contains(QStringLiteral("rs")) && root.contains(QStringLiteral("redPoints"))) activeRedPoints_ = points;
  for (auto entry = opportunities.begin(); entry != opportunities.end(); ++entry) {
    if (entry.key() != kArenaCommand) {
      if (!observedAt_.contains(entry.key())) opportunityPackets_.insert(entry.key(), entry.value());
      continue;
    }
    QJsonObject current = opportunityPackets_.value(kArenaCommand).toObject();
    const QJsonObject candidate = entry.value().toObject();
    for (auto group = candidate.begin(); group != candidate.end(); ++group)
      if (!observedAt_.contains(kArenaCommand + QLatin1Char(':') + group.key())) current.insert(group.key(), group.value());
    opportunityPackets_.insert(kArenaCommand, current);
  }
  return true;
}

QJsonObject RoutineOverviewController::cacheObject() const {
  QJsonArray points;
  QList<int> sorted = activeRedPoints_.values();
  std::sort(sorted.begin(), sorted.end());
  for (int point : sorted) points.append(point);
  return {{QStringLiteral("schema"), 4}, {QStringLiteral("account"), account_},
          {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
          {QStringLiteral("hasDailyPacket"), hasDailyPacket_}, {QStringLiteral("dailyPacket"), dailyPacket_},
          {QStringLiteral("hasRedPointPacket"), hasRedPointPacket_}, {QStringLiteral("redPoints"), points},
          {QStringLiteral("opportunityPackets"), opportunityPackets_}};
}
