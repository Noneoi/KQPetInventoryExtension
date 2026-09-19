#include "application/pet/pet_record_cache.h"
#include <QJsonArray>
#include <cstdio>

namespace {
bool check(bool ok, const char* text) { if (!ok) std::fprintf(stderr, "FAIL: %s\n", text); return ok; }
RawPetRecordInput input(qint64 id, quint64 revision, bool persisted = false) {
  RawPetRecordInput value;
  value.key = {QStringLiteral("account-a"), 1, id, revision};
  value.object = {{QStringLiteral("id"), QString::number(id)}, {QStringLiteral("r"), 7001},
      {QStringLiteral("lv"), 100}, {QStringLiteral("raw"), QString(400, QLatin1Char('x'))}};
  value.complete = true; value.sourceKnown = true; value.persisted = persisted;
  return value;
}
}
int main() {
  const quint64 size = PetRecordCache::measure(input(1, 1).object, {});
  PetRecordCacheLimits limits; limits.maximumBytes = size * 2; limits.maximumRecordBytes = size * 2;
  PetRecordCache cache(limits);
  bool ok = true;
  auto first = cache.reserve({input(1, 1)});
  ok &= check(first.accepted, "first raw admitted"); cache.commit(first.records); first.records.clear();
  auto second = cache.reserve({input(2, 2)});
  ok &= check(second.accepted, "second raw admitted"); cache.commit(second.records); second.records.clear();
  auto failed = cache.reserve({input(3, 3)});
  ok &= check(!failed.accepted && cache.stats().residentRecords == 2 && cache.stats().protectedRecords == 2,
      "unsaved originals cannot be evicted to hide pressure");
  const PetRecordKey firstKey{QStringLiteral("account-a"), 1, 1, 1};
  const PetRecordKey secondKey{QStringLiteral("account-a"), 1, 2, 2};
  auto original = cache.acquire(firstKey.account, firstKey.epoch, firstKey.instanceId);
  auto durable = cache.saved(firstKey, QByteArray(32, 'd'));
  ok &= check(durable && !original->persisted && durable->persisted && durable->payload == original->payload &&
      cache.stats().retainedPayloads == 2, "digest completion freezes a new handle over one payload lease");
  cache.saved(secondKey, QByteArray(32, 'e'));
  cache.markDerived(firstKey);
  // Keep the external handle alive through eviction: removing the LRU entry
  // must not uncharge the pixels/JSON actually retained by its consumer.
  original.reset();
  failed = cache.reserve({input(3, 3)});
  ok &= check(!failed.accepted && !cache.contains(firstKey.account, 1, 1) &&
      durable->object().value(QStringLiteral("raw")).toString().size() == 400 && cache.stats().chargedBytes == size * 2,
      "evicted GUI/Compute handle remains charged and readable");
  durable.reset();
  auto third = cache.reserve({input(3, 3)});
  ok &= check(third.accepted, "releasing the last old handle frees real admission room");
  cache.commit(third.records); third.records.clear();
  ok &= check(cache.stats().chargedBytes <= limits.maximumBytes, "payload byte ceiling held");
  auto duplicate = cache.reserve({input(2, 4), input(2, 5)});
  ok &= check(!duplicate.accepted && cache.acquire(QStringLiteral("account-a"), 1, 2)->key == secondKey,
      "duplicate transactional input leaves old target untouched");
  auto held = cache.acquire(QStringLiteral("account-a"), 1, 3);
  auto changed = cache.revise(held->key, {QStringLiteral("account-a"), 1, 3, 7},
      {{QStringLiteral("lv"), 101}}, false, false);
  ok &= check(changed && changed->payload == held->payload && changed->key.detailMemoryRevision == 7 &&
      !changed->sourceKnown && !changed->rawProjectionOnly && held->key.detailMemoryRevision == 3,
      "brief change advances frozen memory key without copying full original");
  ok &= check(!cache.markDerived(held->key) && cache.markDerived(changed->key), "old derivation cannot unpin a newer observation");
  PetRecordCache other(limits); other.commit({changed});
  ok &= check(other.stats().residentRecords == 0, "foreign lease cannot bypass another cache budget");
  held.reset();
  cache.clearScope();
  ok &= check(cache.stats().residentRecords == 0 && cache.stats().chargedBytes == size &&
      changed->key.account == QStringLiteral("account-a"), "account clearing preserves old leased identity and charge");
  changed.reset();
  ok &= check(cache.stats().chargedBytes == 0 && cache.stats().retainedPayloads == 0, "all borrowed payloads release after consumers finish");
  PetRecordCacheLimits weakLimits = limits;
  weakLimits.maximumBytes = size;
  PetRecordCache weakCache(weakLimits);
  auto weakInput = input(1, 10);
  weakInput.sourceKnown = false;
  auto weak = weakCache.reserve({weakInput});
  ok &= check(weak.accepted, "session-only original admitted");
  weakCache.commit(weak.records); weak.records.clear();
  auto nextWeak = input(2, 11); nextWeak.sourceKnown = false;
  ok &= check(!weakCache.reserve({nextWeak}).accepted && weakCache.stats().protectedRecords == 1,
      "session-only original remains protected until derivation finishes");
  auto borrowedWeak = weakCache.acquire(weakInput.key.account, weakInput.key.epoch, weakInput.key.instanceId);
  weakCache.markDerived(weakInput.key);
  ok &= check(weakCache.stats().protectedRecords == 0 && !weakCache.reserve({nextWeak}).accepted &&
      weakCache.stats().residentRecords == 0 && weakCache.stats().chargedBytes == size && !borrowedWeak->persisted,
      "derived session-only original is reclaimable but borrowed payload stays charged and unsaved");
  borrowedWeak.reset();
  auto continuedWeak = weakCache.reserve({nextWeak});
  ok &= check(continuedWeak.accepted && !continuedWeak.records.first()->persisted &&
      !continuedWeak.records.first()->sourceKnown,
      "session-only raw eviction allows the next read without fabricating a save or source evidence");
  PetRecordCacheLimits depth; depth.maximumJsonDepth = 1;
  ok &= check(!PetRecordCache::measure({{QStringLiteral("a"), QJsonArray{QJsonArray{1}}}}, depth), "meter refuses excessive nesting");
  if (ok) std::puts("PASS: raw record identity, bounded leases, protected originals, LRU, frozen digests and account lifetime");
  return ok ? 0 : 1;
}
