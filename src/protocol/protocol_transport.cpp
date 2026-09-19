#include "protocol_transport.h"

#include "packet_contract.h"

#include <chrono>
#include <limits>

SendPermit::SendPermit() : shared_(std::make_shared<Shared>()) {}
bool SendPermit::claim(qint64 dispatchedAtMs) const {
  if (dispatchedAtMs < 0) return false;
  SendPermitState expected = SendPermitState::Pending;
  if (!shared_->state.compare_exchange_strong(expected, SendPermitState::Claimed,
                                              std::memory_order_acq_rel)) return false;
  shared_->dispatchedAtMs.store(dispatchedAtMs, std::memory_order_release);
  return true;
}
SubmissionOutcome SendPermit::revoke() const {
  SendPermitState expected = SendPermitState::Pending;
  if (shared_->state.compare_exchange_strong(expected, SendPermitState::Revoked,
                                              std::memory_order_acq_rel) ||
      expected == SendPermitState::Revoked || expected == SendPermitState::DefinitelyNotSubmitted)
    return SubmissionOutcome::DefinitelyNotSubmitted;
  return SubmissionOutcome::Unknown;
}
void SendPermit::complete(SubmissionOutcome outcome) const {
  SendPermitState expected = SendPermitState::Claimed;
  const auto next = outcome == SubmissionOutcome::Submitted ? SendPermitState::Submitted
      : outcome == SubmissionOutcome::DefinitelyNotSubmitted ? SendPermitState::DefinitelyNotSubmitted : SendPermitState::Unknown;
  shared_->state.compare_exchange_strong(expected, next, std::memory_order_acq_rel);
}
SendPermitState SendPermit::state() const { return shared_->state.load(std::memory_order_acquire); }
qint64 SendPermit::dispatchedAtMs() const { return shared_->dispatchedAtMs.load(std::memory_order_acquire); }

quint64 nextTransportTaskId() {
  static std::atomic<quint64> next{0};
  quint64 current = next.load(std::memory_order_relaxed);
  do {
    if (current == std::numeric_limits<quint64>::max()) return 0;
  } while (!next.compare_exchange_weak(current, current + 1, std::memory_order_relaxed));
  return current + 1;
}
qint64 transportMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
qint64 remainingResponseMs(const SendReceipt& receipt, qint64 nowMs) {
  return receipt.responseDeadlineMs > nowMs ? receipt.responseDeadlineMs - nowMs : 0;
}

std::optional<SendReceipt> pollSendReceipt(const OutboundIntent& intent, qint64 nowMs) {
  const auto state = intent.permit.state();
  const auto& ticket = intent.ticket;
  const qint64 dispatched = intent.permit.dispatchedAtMs();
  SendReceipt receipt{ticket.taskId, ticket.account, ticket.sessionEpoch, ticket.command};
  if (state == SendPermitState::Pending && nowMs >= ticket.dispatchDeadlineMs) {
    receipt.outcome = intent.permit.revoke();
    receipt.error = QStringLiteral("queued send deadline expired; unclaimed permit revoked");
  } else if (state == SendPermitState::Revoked || state == SendPermitState::DefinitelyNotSubmitted) {
    receipt.outcome = SubmissionOutcome::DefinitelyNotSubmitted;
    receipt.error = QStringLiteral("send permit revoked or definitely not submitted");
  } else if (state == SendPermitState::Submitted || state == SendPermitState::Unknown ||
      (state == SendPermitState::Claimed && dispatched >= 0 && nowMs >=
       (ticket.responseTimeoutMs > 0 ? dispatched + ticket.responseTimeoutMs : ticket.dispatchDeadlineMs))) {
    receipt.outcome = state == SendPermitState::Submitted ? SubmissionOutcome::Submitted : SubmissionOutcome::Unknown;
    if (state == SendPermitState::Claimed) receipt.error = QStringLiteral("claimed host call outcome is unknown");
  } else return {};
  receipt.dispatchedAtMs = dispatched;
  receipt.responseDeadlineMs = dispatched >= 0 ? dispatched + ticket.responseTimeoutMs : -1;
  return receipt;
}

SendReceipt executeIntent(const OutboundIntent& intent, const ActualSendSource& actual,
                          qint64 nowMs, const HostSubmission& submit) {
  SendReceipt receipt{intent.ticket.taskId, intent.ticket.account, intent.ticket.sessionEpoch,
                      intent.ticket.command};
  const auto deny = [&receipt, &intent](const QString& error) {
    receipt.outcome = intent.permit.revoke();
    receipt.error = error;
    return receipt;
  };
  QString error;
  bool declaredWrite = false;
  if (!intent.flashMethod.isEmpty()) {
    declaredWrite = true;
    if (intent.ticket.command != intent.flashMethod || !intent.extension.isEmpty() || !intent.parameters.isEmpty() ||
        !PacketContracts::validateFlash(intent.flashMethod, intent.flashArgument, &error))
      return deny(QStringLiteral("invalid registered Flash intent: %1").arg(error));
  } else {
    const PacketContract* contract = PacketContracts::find(intent.ticket.command);
    if (!contract || !PacketContracts::validateOutbound(intent.extension, intent.ticket.command, intent.parameters, &error))
      return deny(QStringLiteral("invalid registered protocol intent: %1").arg(error));
    declaredWrite = contract->access == PacketAccess::Write;
  }
  if (!intent.ticket.taskId || intent.write != declaredWrite || nowMs < 0 ||
      intent.ticket.responseTimeoutMs < 0 || intent.ticket.responseTimeoutMs > std::numeric_limits<qint64>::max() - nowMs ||
      !submit || actual.closing || nowMs >= intent.ticket.dispatchDeadlineMs)
    return deny(QStringLiteral("intent expired, closing, or has invalid execution metadata"));
  const bool weakRead = !declaredWrite && actual.allowUnverifiedRead && !intent.source.verified() &&
      (!actual.sourceVerifiedAtExecution || (actual.source.account == intent.ticket.account &&
                                             actual.sessionEpoch == intent.ticket.sessionEpoch));
  const bool weakWrite = declaredWrite && actual.allowUnverifiedWrite && !intent.source.verified() &&
      !actual.sourceVerifiedAtExecution && intent.preflightRevision != 0;
  if (!weakRead && !weakWrite && (!actual.sourceVerifiedAtExecution || !actual.source.verified() ||
      !actual.source.sameSource(intent.source) || actual.source.account != intent.ticket.account ||
      actual.sessionEpoch != intent.ticket.sessionEpoch))
    return deny(QStringLiteral("actual GUI source does not match captured account/session evidence"));
  if (intent.write && !weakWrite && (!actual.orderedSubmission || !actual.source.orderingVerified ||
                       !intent.preflightRevision || actual.preflightRevision != intent.preflightRevision))
    return deny(QStringLiteral("write submission has no current source/order/preflight authority"));
  if (!intent.permit.claim(nowMs)) {
    receipt.outcome = intent.permit.revoke();
    receipt.error = QStringLiteral("send permit was revoked or already claimed");
    return receipt;
  }
  receipt.dispatchedAtMs = nowMs;
  if (weakRead) receipt.error = QStringLiteral("read-only unverified observation; no source/order authority granted");
  receipt.responseDeadlineMs = nowMs + intent.ticket.responseTimeoutMs;
  // Exceptions after claiming cannot prove non-submission. The original
  // durable write intent remains Unknown and must never trigger fallback.
  try { receipt.outcome = submit(intent); }
  catch (...) { receipt.outcome = SubmissionOutcome::Unknown; receipt.error = QStringLiteral("host submission raised an exception"); }
  intent.permit.complete(receipt.outcome);
  return receipt;
}

OutboundQueue::OutboundQueue(std::size_t capacity, qint64 maximumBytes)
    : capacity_(capacity), maximumBytes_(maximumBytes) {}
bool OutboundQueue::tryPush(const OutboundIntent& intent) {
  qint64 bytes = 256;
  for (const QString* value : {&intent.ticket.account, &intent.ticket.command, &intent.extension,
      &intent.parameters, &intent.flashMethod, &intent.flashArgument, &intent.source.account,
      &intent.source.sourceId, &intent.source.evidenceReference}) {
    if (maximumBytes_ < bytes || value->size() > (maximumBytes_ - bytes) / 2) return false;
    bytes += value->size() * 2;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!intent.ticket.taskId || intent.permit.state() != SendPermitState::Pending ||
      admitted_.count(intent.ticket.taskId) || admitted_.size() >= capacity_ || bytes < 0 ||
      maximumBytes_ < bytes || bytes_ > maximumBytes_ - bytes) return false;
  queued_.push_back(intent);
  admitted_.emplace(intent.ticket.taskId, Admitted{intent.permit, bytes});
  bytes_ += bytes;
  return true;
}
std::optional<OutboundIntent> OutboundQueue::takeNext() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queued_.empty()) return {};
  OutboundIntent intent = std::move(queued_.front());
  queued_.pop_front();
  return intent;
}
void OutboundQueue::complete(quint64 taskId) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto entry = admitted_.find(taskId);
  if (entry == admitted_.end()) return;
  entry->second.permit.revoke();
  for (auto queued = queued_.begin(); queued != queued_.end();) {
    if (queued->ticket.taskId == taskId) queued = queued_.erase(queued);
    else ++queued;
  }
  bytes_ -= entry->second.bytes;
  admitted_.erase(entry);
}
void OutboundQueue::revokeAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& item : admitted_) item.second.permit.revoke();
}
std::size_t OutboundQueue::outstanding() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return admitted_.size();
}
