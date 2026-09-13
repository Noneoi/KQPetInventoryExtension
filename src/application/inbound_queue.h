#pragma once
#include "session_context.h"
#include <QList>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

struct InboundQueueLimits {
  int maximumPackets = 256;
  qint64 maximumBytes = 32 * 1024 * 1024;
  qint64 maximumPacketBytes = 16 * 1024 * 1024;
};
struct InboundQueueState {
  int retainedPackets = 0;
  qint64 retainedBytes = 0;
  int queuedPackets = 0;
  bool uncertain = false;
};

class InboundQueue final {
public:
  explicit InboundQueue(const InboundQueueLimits& limits = {});
  bool push(InboundEnvelope envelope);
  QList<InboundEnvelope> takeBatch(int maximumPackets = 16, qint64 maximumBytes = 1024 * 1024);
  void fail();
  bool healthy() const;
  InboundQueueState state() const;
private:
  struct Budget;
  InboundQueueLimits limits_;
  std::shared_ptr<Budget> budget_;
  mutable std::mutex mutex_;
  std::deque<InboundEnvelope> queue_;
  quint64 nextSequence_ = 0;
};
