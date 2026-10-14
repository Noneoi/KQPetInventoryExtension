#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QStringList>

// Plain values. Reading receipts, catalogs or change files belongs to Storage
// Auxiliary; session application is supplied by the current Core process.
enum class PublicUpdatePhase { Idle, PreparingTools, FetchingResources, Parsing, Validating, Applying };
enum class PublicUpdateOutcome { NotChecked, Checking, Unchanged, Applied, Partial, KeptPrevious, Cancelled, PendingApply, Restored };

struct PublicUpdateProgress {
  PublicUpdatePhase phase = PublicUpdatePhase::Idle;
  QString attempt;
  QString component;
  QString message;
  qint64 completed = -1;
  qint64 total = -1;
};

struct PublicUpdateComponentState {
  QString key;
  QString label;
  QString status; // updated / unchanged / retained / failed / notChecked
  QString message;
  bool retryable = false;
  bool hasWarnings = false;
  QJsonObject technical;
};

struct PublicUpdateChange {
  QString component;
  QString category;
  QString kind; // added / changed / removed
  QString name;
  QString beforeName;
  QString afterName;
  QString beforeText;
  QString afterText;
  QJsonObject technical; // Exact namespace/identifier, only for folded details.
};

struct PublicUpdateState {
  bool readable = true;
  bool busy = false;
  bool canCancel = false;
  bool canRestorePrevious = false;
  bool sessionApplied = false;
  bool candidatePassed = false;
  bool diskCommitted = false;
  PublicUpdateOutcome outcome = PublicUpdateOutcome::NotChecked;
  PublicUpdateProgress progress;
  QString latestAttempt;
  QString checkedUtc;
  QString diskRevision;
  QString sessionRevision;
  QString previousRevision;
  QString summary;
  QString error;
  QStringList requestedComponents;
  QStringList effectiveComponents;
  QStringList retryComponents;
  QList<PublicUpdateComponentState> components;
  QList<PublicUpdateChange> changes;
  qint64 totalChanges = 0;
  bool changesRecorded = false;
  bool changesTruncated = false;
  qint64 pictureCompleted = -1;
  qint64 pictureTotal = -1;
  QJsonObject technical;
};

Q_DECLARE_METATYPE(PublicUpdateProgress)
Q_DECLARE_METATYPE(PublicUpdateState)
