#pragma once
#include <QMetaType>
#include <QString>

// Host acceptance is not a server acknowledgement. Only DefinitelyNotSubmitted
// permits another transport; Unknown requires read-only reconciliation.
enum class SubmissionOutcome { DefinitelyNotSubmitted, Submitted, Unknown };
enum class MoveOutcome { NotSent, Submitted, Confirmed, Rejected, Unknown };
Q_DECLARE_METATYPE(SubmissionOutcome)
Q_DECLARE_METATYPE(MoveOutcome)

inline QString moveOutcomeName(MoveOutcome outcome) {
  switch (outcome) {
    case MoveOutcome::NotSent: return QStringLiteral("NotSent");
    case MoveOutcome::Submitted: return QStringLiteral("Submitted");
    case MoveOutcome::Confirmed: return QStringLiteral("Confirmed");
    case MoveOutcome::Rejected: return QStringLiteral("Rejected");
    case MoveOutcome::Unknown: return QStringLiteral("Unknown");
  }
  return QStringLiteral("Unknown");
}
