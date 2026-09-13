#include "observation_freshness.h"
#include "protocol_transport.h"

#include <QTimeZone>
#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
constexpr qint64 maximumUtcMs = 253402300799999LL; // year 9999; checked arithmetic domain
bool utcInstant(const QDateTime& value) {
  return value.isValid() && value.timeSpec() == Qt::UTC && value.toMSecsSinceEpoch() >= 0 &&
      value.toMSecsSinceEpoch() <= maximumUtcMs;
}
bool validName(const QString& value, int maximum) {
  if (value.isEmpty() || value.size() > maximum) return false;
  for (QChar character : value) if (character.unicode() < 32) return false;
  return true;
}
qint64 distance(qint64 a, qint64 b) { return a >= b ? a - b : b - a; }
}

ObservationFreshness::ObservationFreshness(ObservationClock clock)
    : clock_(clock ? std::move(clock) : ObservationClock(&ObservationFreshness::systemClock)) {}

ObservationClockSample ObservationFreshness::systemClock() {
  const qint64 before = transportMonotonicMs();
  QDateTime utc;
#ifdef Q_OS_WIN
  FILETIME fileTime{};
  GetSystemTimePreciseAsFileTime(&fileTime);
  ULARGE_INTEGER ticks{}; ticks.LowPart = fileTime.dwLowDateTime; ticks.HighPart = fileTime.dwHighDateTime;
  constexpr quint64 unixEpoch = 116444736000000000ULL;
  if (ticks.QuadPart >= unixEpoch)
    utc = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>((ticks.QuadPart - unixEpoch) / 10000), Qt::UTC);
#else
  utc = QDateTime::currentDateTimeUtc();
#endif
  const qint64 after = transportMonotonicMs();
  // Bracket the wall sample and include the two millisecond quantizations.
  // This is a measurement-error bound, never a freshness TTL.
  const qint64 uncertainty = after >= before && after - before <= maximumUtcMs ? after - before + 2 : -1;
  const QByteArray zoneName = QTimeZone::systemTimeZoneId();
  const QString zone = zoneName.isEmpty() ? QString{} : QString::fromUtf8(zoneName) + QLatin1Char('/') +
      QString::number(utc.toLocalTime().offsetFromUtc());
  return {utc, after, uncertainty, zone};
}

bool ObservationFreshness::validClock(const ObservationClockSample& sample) {
  return utcInstant(sample.utc) && sample.monotonicMs >= 0 && sample.uncertaintyMs >= 0 &&
      sample.uncertaintyMs <= maximumUtcMs && validName(sample.zoneId, 256);
}
QString ObservationFreshness::key(const QString& group, const QString& periodKey) {
  return group + QChar(0x1f) + periodKey;
}
void ObservationFreshness::setClock(ObservationClock clock) {
  clock_ = clock ? std::move(clock) : ObservationClock(&ObservationFreshness::systemClock);
  hasAnchor_ = false; invalidClockSeen_ = false;
  changeClock(QStringLiteral("观察时钟来源已改变"), -1);
  checked_ = {};
}
void ObservationFreshness::bindSession(const QString& account, quint64 epoch) {
  if (account_ == account && epoch_ == epoch) return;
  account_ = account; epoch_ = epoch;
  observations_.clear(); evidence_.clear();
  sourceInvalidated_ = false; sourceReason_.clear();
  ++sourceGeneration_; ++revision_;
}
void ObservationFreshness::changeClock(const QString& reason, qint64 monotonicMs) {
  ++clockGeneration_; ++revision_;
  clockReason_ = reason;
  clockBoundaryMonotonicMs_ = monotonicMs;
}
void ObservationFreshness::sampleClock() {
  ObservationClockSample now;
  try { now = clock_(); } catch (...) { now = {}; }
  checked_ = now;
  if (!validClock(now)) {
    if (!invalidClockSeen_) changeClock(QStringLiteral("观察时钟数据无效"), previous_.monotonicMs);
    invalidClockSeen_ = true; hasAnchor_ = false;
    return;
  }
  if (!hasAnchor_) {
    anchor_ = previous_ = now; hasAnchor_ = true; invalidClockSeen_ = false;
    return;
  }
  const qint64 anchorWall = anchor_.utc.toMSecsSinceEpoch();
  const qint64 wall = now.utc.toMSecsSinceEpoch();
  QString reason;
  if (now.monotonicMs < previous_.monotonicMs || now.monotonicMs < anchor_.monotonicMs)
    reason = QStringLiteral("单调时钟回退，旧观察失效");
  else if (now.zoneId != anchor_.zoneId)
    reason = QStringLiteral("系统时区发生变化，旧观察待核验");
  else {
    const qint64 elapsed = now.monotonicMs - anchor_.monotonicMs;
    const qint64 tolerance = anchor_.uncertaintyMs + now.uncertaintyMs;
    if (elapsed > maximumUtcMs || anchorWall > maximumUtcMs - elapsed ||
        distance(wall, anchorWall + elapsed) > tolerance)
      reason = QStringLiteral("墙钟与单调时钟不再一致，旧观察待核验");
  }
  if (!reason.isEmpty()) { changeClock(reason, now.monotonicMs); anchor_ = now; }
  previous_ = now;
}
bool ObservationFreshness::check() {
  const quint64 before = revision_;
  sampleClock();
  for (auto it = evidence_.begin(); it != evidence_.end(); ++it) {
    const auto state = evaluate(it.value().value.group, it.value()).state;
    if (state != it->lastState) { it->lastState = state; ++revision_; }
    if (state == ObservationValidityState::Invalidated) it->invalidated = true;
  }
  return before != revision_;
}
void ObservationFreshness::invalidateObservations(const QString& reason) {
  if (sourceInvalidated_ && sourceReason_ == reason) return;
  sourceInvalidated_ = true; sourceReason_ = reason;
  ++sourceGeneration_; ++revision_;
}
bool ObservationFreshness::observe(const QString& group, quint64 sequence, qint64 capturedMonotonicMs) {
  check();
  if (!validName(account_, 1024) || epoch_ == 0 || !validName(group, 128) || sequence == 0 ||
      (observations_.contains(group) && sequence <= observations_.value(group).sequence) ||
      (!observations_.contains(group) && observations_.size() >= 128)) return false;
  Observed value;
  value.sequence = sequence; value.utc = checked_.utc;
  value.sourceGeneration = sourceGeneration_;
  value.clockGeneration = clockGeneration_;
  value.capturedMonotonicMs = capturedMonotonicMs;
  value.clockUncertaintyMs = checked_.uncertaintyMs;
  value.captureKnown = validClock(checked_) && capturedMonotonicMs >= 0 && capturedMonotonicMs <= checked_.monotonicMs;
  if (!validClock(checked_) || (clockBoundaryMonotonicMs_ >= 0 && capturedMonotonicMs < clockBoundaryMonotonicMs_))
    value.clockGeneration = 0;
  observations_.insert(group, value);
  for (auto it = evidence_.begin(); it != evidence_.end();)
    if (it->value.group == group) it = evidence_.erase(it); else ++it;
  sourceInvalidated_ = false;
  ++revision_;
  return true;
}
ObservationValidity ObservationFreshness::observation(const QString& group) const {
  ObservationValidity result;
  const auto observed = observations_.constFind(group);
  if (observed == observations_.cend()) { result.reason = QStringLiteral("尚无此项观察"); return result; }
  result.observationSequence = observed->sequence; result.observedAtUtc = observed->utc;
  if (!validClock(checked_) || observed->clockGeneration != clockGeneration_) {
    result.state = ObservationValidityState::Invalidated;
    result.reason = clockReason_.isEmpty() ? QStringLiteral("观察时间无法核验") : clockReason_;
  } else if (observed->sourceGeneration != sourceGeneration_) {
    result.state = ObservationValidityState::Invalidated;
    result.reason = sourceReason_.isEmpty() ? QStringLiteral("会话来源变化，旧观察失效") : sourceReason_;
  } else {
    result.state = ObservationValidityState::Current;
    result.reason = QStringLiteral("只读观察；不代表限次周期已核验");
  }
  return result;
}
ObservationValidity ObservationFreshness::evaluate(const QString& group, const Evidence& evidence) const {
  auto result = observation(group);
  result.periodId = evidence.value.periodId;
  result.evidenceReference = evidence.value.evidenceReference;
  result.validUntilUtc = evidence.value.validUntilUtc;
  result.expiresMonotonicMs = evidence.expiresMonotonicMs;
  if (result.state != ObservationValidityState::Current) return result;
  result.observedAtUtc = evidence.value.observedServerUtc;
  if (evidence.invalidated || checked_.monotonicMs >= evidence.expiresMonotonicMs) {
    result.state = ObservationValidityState::Invalidated;
    result.reason = evidence.invalidReason.isEmpty()
        ? QStringLiteral("已超过核验过的限次周期；旧次数不再参与判断") : evidence.invalidReason;
    return result;
  }
  result.reason = QStringLiteral("周期%1已核验，有效至%2")
      .arg(result.periodId, result.validUntilUtc.toString(Qt::ISODateWithMs));
  return result;
}
ObservationValidity ObservationFreshness::status(const QString& group, const QString& periodKey) const {
  const auto evidence = evidence_.constFind(key(group, periodKey));
  if (evidence != evidence_.cend()) return evaluate(group, evidence.value());
  auto result = observation(group);
  if (result.state == ObservationValidityState::Current) result.state = ObservationValidityState::Unknown;
  if (result.state != ObservationValidityState::Invalidated)
    result.reason = QStringLiteral("周期未核验；此数值仅为只读观察");
  return result;
}
bool ObservationFreshness::acceptValidityEvidence(const TrustedObservationValidity& value, QString* error) {
  check();
  const auto fail = [error](const QString& reason) { if (error) *error = reason; return false; };
  const auto observed = observations_.constFind(value.group);
  if (value.account != account_ || value.epoch != epoch_ || observed == observations_.cend() ||
      value.observationSequence != observed->sequence || !observed->captureKnown ||
      observation(value.group).state != ObservationValidityState::Current ||
      !validName(value.periodKey, 32) || !validName(value.periodId, 256) || !validName(value.evidenceReference, 1024))
    return fail(QStringLiteral("周期证据没有绑定当前账号会话及这次可信观察"));
  if (!utcInstant(value.validFromUtc) || !utcInstant(value.validUntilUtc) || !utcInstant(value.observedServerUtc) ||
      value.serverUncertaintyMs < 0 || value.serverUncertaintyMs > maximumUtcMs)
    return fail(QStringLiteral("周期边界或服务器观察时间无效"));
  const qint64 from = value.validFromUtc.toMSecsSinceEpoch(), until = value.validUntilUtc.toMSecsSinceEpoch();
  const qint64 server = value.observedServerUtc.toMSecsSinceEpoch();
  const qint64 uncertainty = value.serverUncertaintyMs;
  if (from >= until || server < from || server - from < uncertainty || server >= until || until - server <= uncertainty)
    return fail(QStringLiteral("服务器观察时间不在可证明的周期范围内"));
  const qint64 remaining = until - server - uncertainty - observed->clockUncertaintyMs;
  if (remaining <= 0) return fail(QStringLiteral("时钟误差覆盖周期边界，不能确认仍在有效期内"));
  if (observed->capturedMonotonicMs > std::numeric_limits<qint64>::max() - remaining)
    return fail(QStringLiteral("单调时钟期限溢出"));
  const qint64 expiry = observed->capturedMonotonicMs + remaining;
  if (checked_.monotonicMs >= expiry)
    return fail(QStringLiteral("周期证据到达时已经过期"));
  const QString entry = key(value.group, value.periodKey);
  auto previous = evidence_.find(entry);
  if (previous != evidence_.end()) {
    const auto& old = previous->value;
    if (old.periodId != value.periodId || old.validFromUtc != value.validFromUtc ||
        old.validUntilUtc != value.validUntilUtc || old.observedServerUtc != value.observedServerUtc ||
        old.serverUncertaintyMs != value.serverUncertaintyMs) {
      previous->invalidated = true;
      previous->invalidReason = QStringLiteral("同次观察的周期证据冲突，等待新的可信观察");
      previous->lastState = ObservationValidityState::Invalidated;
      ++revision_;
      return fail(previous->invalidReason);
    }
    return !previous->invalidated;
  }
  if (!evidence_.contains(entry) && evidence_.size() >= 512)
    return fail(QStringLiteral("周期证据缓存已满"));
  Evidence accepted;
  accepted.value = value; accepted.expiresMonotonicMs = expiry;
  accepted.lastState = ObservationValidityState::Current;
  evidence_.insert(entry, accepted);
  ++revision_;
  return true;
}
quint64 ObservationFreshness::observationSequence(const QString& group) const { return observations_.value(group).sequence; }
QDateTime ObservationFreshness::observedAt(const QString& group) const { return observations_.value(group).utc; }
