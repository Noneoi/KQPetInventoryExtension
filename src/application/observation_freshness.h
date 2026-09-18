#pragma once
#include "../contracts/observation_types.h"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <functional>

struct ObservationClockSample {
  QDateTime utc;
  qint64 monotonicMs = -1;
  qint64 uncertaintyMs = 0;
  QString zoneId;
};
using ObservationClock = std::function<ObservationClockSample()>;

// This is an Application capability supplied by a reviewed protocol adapter,
// not a field that JSON, UI or a file may self-declare as trusted. No production
// source currently proves this complete contract for the quota observations.
struct TrustedObservationValidity {
  QString account;
  quint64 epoch = 0;
  QString group;
  quint64 observationSequence = 0;
  QString periodKey;
  QString periodId;
  QDateTime validFromUtc;
  QDateTime validUntilUtc;
  QDateTime observedServerUtc;
  // Includes server timestamp precision and its transport-to-capture error.
  qint64 serverUncertaintyMs = -1;
  QString evidenceReference;
};

// Core-owned, no timer, I/O, server request or local-midnight/TTL policy. Call
// check at capture, publication and from the Runtime's bounded periodic tick.
class ObservationFreshness final {
public:
  explicit ObservationFreshness(ObservationClock clock = {});
  static ObservationClockSample systemClock();
  void setClock(ObservationClock clock);
  void bindSession(const QString& account, quint64 epoch);
  bool observe(const QString& group, quint64 sequence, qint64 capturedMonotonicMs = -1);
  bool acceptValidityEvidence(const TrustedObservationValidity& evidence, QString* error = nullptr);
  bool check();
  void invalidateObservations(const QString& reason);
  ObservationValidity status(const QString& group, const QString& periodKey) const;
  ObservationValidity observation(const QString& group) const;
  quint64 revision() const { return revision_; }
  quint64 observationSequence(const QString& group) const;
  QDateTime observedAt(const QString& group) const;
  int observationCount() const { return observations_.size(); }
  int evidenceCount() const { return evidence_.size(); }

private:
  struct Observed {
    quint64 sequence = 0, clockGeneration = 0, sourceGeneration = 0;
    QDateTime utc;
    qint64 capturedMonotonicMs = -1;
    qint64 clockUncertaintyMs = 0;
    bool captureKnown = false;
  };
  struct Evidence {
    TrustedObservationValidity value;
    qint64 expiresMonotonicMs = -1;
    bool invalidated = false;
    QString invalidReason;
    ObservationValidityState lastState = ObservationValidityState::Unknown;
  };
  static QString key(const QString& group, const QString& periodKey);
  static bool validClock(const ObservationClockSample& sample);
  ObservationValidity evaluate(const QString& group, const Evidence& evidence) const;
  void sampleClock();
  void changeClock(const QString& reason, qint64 monotonicMs);
  ObservationClock clock_;
  ObservationClockSample checked_, anchor_, previous_;
  QString account_, clockReason_, sourceReason_;
  quint64 epoch_ = 0, revision_ = 0, clockGeneration_ = 1, sourceGeneration_ = 1;
  qint64 clockBoundaryMonotonicMs_ = -1;
  bool hasAnchor_ = false, invalidClockSeen_ = false, sourceInvalidated_ = false;
  QHash<QString, Observed> observations_;
  QHash<QString, Evidence> evidence_;
};
