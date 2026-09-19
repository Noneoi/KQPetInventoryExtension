#include "read_only_request_tracker.h"

bool ReadOnlyRequestTracker::queue(const AsyncSender& sender, const QString& account, quint64 sessionEpoch,
                                   const SessionSourceEvidence& source, const QString& extension,
                                   const QString& command, const QString& parameters) {
  for (const auto& intent : std::as_const(pendingSends_))
    if (intent.ticket.command == command) return true;
  OutboundIntent intent;
  intent.ticket = {nextTransportTaskId(), account, sessionEpoch, command, 0,
                   transportMonotonicMs() + 10000, 10000, PacketCorrelationStrength::CommandObserved};
  intent.source = source;
  intent.extension = extension; intent.parameters = parameters;
  pendingSends_.insert(intent.ticket.taskId, intent);
  if (sender(intent)) return true;
  intent.permit.revoke(); pendingSends_.remove(intent.ticket.taskId);
  return false;
}

void ReadOnlyRequestTracker::revokeAll() {
  for (const auto& intent : std::as_const(pendingSends_)) intent.permit.revoke();
  pendingSends_.clear(); responseDeadlines_.clear();
  requestDispatchTimes_.clear();
}

std::optional<OutboundIntent> ReadOnlyRequestTracker::takeIntent(const SendReceipt& receipt) {
  const auto found = pendingSends_.find(receipt.taskId);
  if (found == pendingSends_.end()) return std::nullopt;
  const OutboundIntent intent = found.value();
  if (receipt.account != intent.ticket.account || receipt.sessionEpoch != intent.ticket.sessionEpoch ||
      receipt.command != intent.ticket.command) return std::nullopt;
  pendingSends_.erase(found);
  return intent;
}

bool ReadOnlyRequestTracker::recordDispatch(const QString& command, qint64 dispatchedAtMs,
                                            qint64 responseTimeoutMs) {
  requestDispatchTimes_.insert(command, dispatchedAtMs);
  const qint64 deadline = dispatchedAtMs + responseTimeoutMs;
  if (deadline <= transportMonotonicMs()) return false;
  responseDeadlines_.insert(command, deadline);
  return true;
}

void ReadOnlyRequestTracker::markDispatchedNow(const QString& command) {
  requestDispatchTimes_.insert(command, transportMonotonicMs());
}

void ReadOnlyRequestTracker::forgetDispatch(const QString& command) {
  requestDispatchTimes_.remove(command);
}

void ReadOnlyRequestTracker::forget(const QString& command) {
  responseDeadlines_.remove(command);
  requestDispatchTimes_.remove(command);
  for (auto pending = pendingSends_.begin(); pending != pendingSends_.end();) {
    if (pending.value().ticket.command == command) {
      pending.value().permit.revoke(); pending = pendingSends_.erase(pending);
    } else ++pending;
  }
}

qint64 ReadOnlyRequestTracker::dispatchedAt(const QString& command) const {
  qint64 dispatched = requestDispatchTimes_.value(command, -1);
  for (const auto& intent : pendingSends_) {
    if (intent.ticket.command != command) continue;
    const auto state = intent.permit.state();
    if (state == SendPermitState::Claimed || state == SendPermitState::Submitted ||
        state == SendPermitState::Unknown) dispatched = intent.permit.dispatchedAtMs();
  }
  return dispatched;
}

QList<SendReceipt> ReadOnlyRequestTracker::pollReceipts(qint64 nowMs) const {
  QList<SendReceipt> receipts;
  for (const auto& intent : std::as_const(pendingSends_)) {
    const auto receipt = pollSendReceipt(intent, nowMs);
    if (receipt) receipts.append(*receipt);
  }
  return receipts;
}

QStringList ReadOnlyRequestTracker::expiredCommands(qint64 nowMs) const {
  QStringList expired;
  for (auto deadline = responseDeadlines_.begin(); deadline != responseDeadlines_.end(); ++deadline)
    if (deadline.value() <= nowMs) expired.append(deadline.key());
  return expired;
}
