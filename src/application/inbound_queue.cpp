#include "inbound_queue.h"

struct InboundQueue::Budget {
  std::atomic<int> packets{0};
  std::atomic<qint64> bytes{0};
  std::atomic_bool uncertain{false};
};

InboundQueue::InboundQueue(const InboundQueueLimits& limits)
    : limits_(limits), budget_(std::make_shared<Budget>()) {}

bool InboundQueue::push(InboundEnvelope envelope) {
  // This accounts retained QString UTF-16 storage, independent of JSON UTF-8
  // conversion later on Core. Reservations include already queued GUI/Core copies.
  if (limits_.maximumPacketBytes <= 0 || envelope.payload.size() > limits_.maximumPacketBytes / 2 ||
      envelope.method.size() > limits_.maximumPacketBytes / 2) { fail(); return false; }
  const qint64 bytes = qint64(envelope.payload.size()) * 2 + qint64(envelope.method.size()) * 2;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!healthy()) return false;
  if (bytes <= 0 || bytes > limits_.maximumPacketBytes ||
      budget_->packets.load() >= limits_.maximumPackets ||
      bytes > limits_.maximumBytes - budget_->bytes.load()) {
    budget_->uncertain.store(true, std::memory_order_release);
    queue_.clear();
    return false;
  }
  budget_->packets.fetch_add(1);
  budget_->bytes.fetch_add(bytes);
  envelope.receiveSequence = ++nextSequence_;
  // Capture the budget instead of the queue so shutdown/delayed consumers never
  // dereference a destroyed bridge. Copies share this single reservation.
  envelope.retention = std::shared_ptr<void>(budget_.get(), [budget = budget_, bytes](void*) {
    budget->bytes.fetch_sub(bytes);
    budget->packets.fetch_sub(1);
  });
  queue_.push_back(std::move(envelope));
  return true;
}

QList<InboundEnvelope> InboundQueue::takeBatch(int maximumPackets, qint64 maximumBytes) {
  QList<InboundEnvelope> batch;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!healthy()) { queue_.clear(); return batch; }
  qint64 bytes = 0;
  while (!queue_.empty() && batch.size() < maximumPackets) {
    const qint64 next = qint64(queue_.front().payload.size() + queue_.front().method.size()) * 2;
    if (!batch.isEmpty() && next > maximumBytes - bytes) break;
    bytes += next;
    batch.append(std::move(queue_.front()));
    queue_.pop_front();
  }
  return batch;
}
void InboundQueue::fail() {
  budget_->uncertain.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
}
bool InboundQueue::healthy() const { return !budget_->uncertain.load(std::memory_order_acquire); }
InboundQueueState InboundQueue::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {budget_->packets.load(), budget_->bytes.load(), int(queue_.size()), !healthy()};
}
