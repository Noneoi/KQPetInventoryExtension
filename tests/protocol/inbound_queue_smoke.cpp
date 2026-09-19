#include "protocol/inbound_queue.h"
#include <cstdio>

int main() {
  InboundQueueLimits limits;
  limits.maximumPackets = 2; limits.maximumPacketBytes = 1024; limits.maximumBytes = 1024;
  InboundQueue queue(limits);
  InboundEnvelope first; first.receiveSequence = 1; first.method = QStringLiteral("recivedata"); first.payload = QStringLiteral("{}");
  bool ok = queue.push(first);
  auto retainedByCore = queue.takeBatch();
  InboundEnvelope second = first; second.receiveSequence = 2;
  ok = ok && queue.push(second) && queue.state().retainedPackets == 2;
  // Dequeue did not free the reservation while Core still owns an envelope.
  ok = ok && !queue.push(second) && !queue.healthy() && queue.takeBatch().isEmpty();
  retainedByCore.clear();
  ok = ok && queue.state().retainedPackets == 0 && queue.state().retainedBytes == 0;
  InboundQueue large(limits);
  first.payload = QString(600, QLatin1Char('x'));
  ok = ok && !large.push(first) && !large.healthy();
  InboundQueue healthy;
  first.payload = QStringLiteral("{}");
  ok = ok && healthy.push(first);
  auto copy = healthy.takeBatch();
  auto anotherCopy = copy;
  copy.clear();
  ok = ok && healthy.state().retainedPackets == 1;
  anotherCopy.clear();
  ok = ok && healthy.state().retainedPackets == 0;
  std::puts(ok ? "PASS: inbound byte/packet limits, delayed consumers, shared accounting and fail-closed overflow"
               : "FAIL: inbound queue accounting");
  return ok ? 0 : 1;
}
