#pragma once

#include "protocol/protocol_transport.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// In-flight bookkeeping shared by the read-only query controllers (shop and
// routine): the queued send intents, when each command actually left the host
// boundary, and each command's response deadline. It holds no controller or
// session policy; callers decide whether a request may be queued or matched.
class ReadOnlyRequestTracker final {
public:
  // Queues one intent per command. Returns true when the command is already
  // queued or the sender accepted the new intent.
  bool queue(const AsyncSender& sender, const QString& account, quint64 sessionEpoch,
             const SessionSourceEvidence& source, const QString& extension,
             const QString& command, const QString& parameters);
  // Revokes every queued intent and forgets all dispatch times and deadlines.
  void revokeAll();
  // Removes and returns the intent this receipt belongs to. Unknown receipts
  // and receipts for another account/epoch/command return nullopt and keep
  // any queued intent.
  std::optional<OutboundIntent> takeIntent(const SendReceipt& receipt);
  // Records an actual dispatch. Returns false when the response deadline has
  // already passed, in which case no deadline is armed.
  bool recordDispatch(const QString& command, qint64 dispatchedAtMs, qint64 responseTimeoutMs);
  // Synchronous sender path: the command leaves now.
  void markDispatchedNow(const QString& command);
  void forgetDispatch(const QString& command);
  // Forgets one command entirely, revoking its queued intents.
  void forget(const QString& command);
  // Earliest proof that the command crossed its execution boundary, or -1.
  // The host may deliver a response before the queued send receipt reaches
  // Core; a claimed/submitted permit still proves the dispatch.
  qint64 dispatchedAt(const QString& command) const;
  QList<SendReceipt> pollReceipts(qint64 nowMs) const;
  QStringList expiredCommands(qint64 nowMs) const;

private:
  QHash<quint64, OutboundIntent> pendingSends_;
  QHash<QString, qint64> responseDeadlines_;
  QHash<QString, qint64> requestDispatchTimes_;
};
