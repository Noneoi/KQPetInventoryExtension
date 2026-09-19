#pragma once

#include "session_context.h"
#include "contracts/operation_types.h"

#include <QMetaType>
#include <QList>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <unordered_map>

enum class SendPermitState { Pending, Claimed, Submitted, Unknown, DefinitelyNotSubmitted, Revoked };

class SendPermit final {
public:
  SendPermit();
  bool claim(qint64 dispatchedAtMs) const;
  // Only Pending can be revoked with proof of non-submission. Claiming already
  // crossed the execution boundary, so cancellation there returns Unknown.
  SubmissionOutcome revoke() const;
  void complete(SubmissionOutcome outcome) const;
  SendPermitState state() const;
  qint64 dispatchedAtMs() const;
private:
  struct Shared {
    std::atomic<SendPermitState> state{SendPermitState::Pending};
    std::atomic<qint64> dispatchedAtMs{-1};
  };
  std::shared_ptr<Shared> shared_;
};

struct RequestTicket {
  quint64 taskId = 0;
  QString account;
  quint64 sessionEpoch = 0;
  QString command;
  qint64 entityId = 0;
  qint64 dispatchDeadlineMs = 0;
  qint64 responseTimeoutMs = 0;
  PacketCorrelationStrength correlationStrength = PacketCorrelationStrength::CommandObserved;
};

struct OutboundIntent {
  RequestTicket ticket;
  SessionSourceEvidence source;
  quint64 preflightRevision = 0;
  QString extension;
  QString parameters;
  QString flashMethod;
  QString flashArgument;
  bool write = false;
  SendPermit permit;
};

struct SendReceipt {
  quint64 taskId = 0;
  QString account;
  quint64 sessionEpoch = 0;
  QString command;
  SubmissionOutcome outcome = SubmissionOutcome::DefinitelyNotSubmitted;
  qint64 dispatchedAtMs = -1;
  qint64 responseDeadlineMs = -1;
  QString error;
};

// Supplied from the GUI's actual host connection/page boundary at execution,
// never copied from the Core's currently selected account as a substitute.
struct ActualSendSource {
  SessionSourceEvidence source;
  quint64 sessionEpoch = 0;
  quint64 preflightRevision = 0;
  bool closing = false;
  bool sourceVerifiedAtExecution = false;
  bool orderedSubmission = false;
  // Explicit capability of a compatible, healthy host process. This permits
  // registered reads as unverified observations, never account attribution or writes.
  // Writes need allowUnverifiedWrite below.
  bool allowUnverifiedRead = false;
  // Explicit capability for a registered write (the pet move) from a session
  // without verified source evidence. Core only queues such a write after a
  // fresh, complete, in-order list preflight on an uninterrupted read stream
  // (PetRepository::readContinuityWriteAllowed), and revokes its permit on any
  // account/session change. It never covers an intent carrying verified source
  // evidence: those still need matching source and host order at execution.
  bool allowUnverifiedWrite = false;
};

using AsyncSender = std::function<bool(const OutboundIntent&)>;
using HostSubmission = std::function<SubmissionOutcome(const OutboundIntent&)>;
quint64 nextTransportTaskId();
qint64 transportMonotonicMs();
qint64 remainingResponseMs(const SendReceipt& receipt, qint64 nowMs);
std::optional<SendReceipt> pollSendReceipt(const OutboundIntent& intent, qint64 nowMs);
SendReceipt executeIntent(const OutboundIntent& intent, const ActualSendSource& actual,
                          qint64 nowMs, const HostSubmission& submit);

// One shared queue is owned by ApplicationRuntime. Outstanding admission
// includes taken-but-not-completed entries, so a blocked GUI cannot hide work
// from either memory accounting or an independent revokeAll() call.
class OutboundQueue final {
public:
  explicit OutboundQueue(std::size_t capacity = 256, qint64 maximumBytes = 1024 * 1024);
  bool tryPush(const OutboundIntent& intent);
  std::optional<OutboundIntent> takeNext();
  void complete(quint64 taskId);
  void revokeAll();
  std::size_t outstanding() const;
private:
  struct Admitted { SendPermit permit; qint64 bytes = 0; };
  mutable std::mutex mutex_;
  std::deque<OutboundIntent> queued_;
  std::unordered_map<quint64, Admitted> admitted_;
  std::size_t capacity_;
  qint64 maximumBytes_;
  qint64 bytes_ = 0;
};

Q_DECLARE_METATYPE(OutboundIntent)
Q_DECLARE_METATYPE(SendReceipt)
