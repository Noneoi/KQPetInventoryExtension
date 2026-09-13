#pragma once
#include <QDateTime>
#include <QString>

enum class ObservationValidityState { Unknown, Current, Invalidated };

// Read-only presentation of Core's period decision. It grants no protocol or
// persistence capability; the evidence issuer remains inside Application.
struct ObservationValidity {
  ObservationValidityState state = ObservationValidityState::Unknown;
  QString reason;
  QString evidenceReference;
  QString periodId;
  QDateTime observedAtUtc;
  QDateTime validUntilUtc;
  quint64 observationSequence = 0;
  qint64 expiresMonotonicMs = -1;
  bool current() const { return state == ObservationValidityState::Current; }
};
