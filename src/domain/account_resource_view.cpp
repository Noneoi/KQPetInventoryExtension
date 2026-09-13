#include "account_resource_view.h"

AccountResourceView::AccountResourceView(
    const QHash<QString, qint64>& counts, bool sourceReliable,
    const QDateTime& observedAt, const QString& source, bool invalidated)
    : counts_(counts), sourceReliable_(sourceReliable),
      observedAt_(observedAt.toUTC()), source_(source), invalidated_(invalidated) {}

bool AccountResourceView::hasReliableCount(const QString& key) const {
  return sourceReliable_ && !invalidated_ && counts_.contains(key) &&
         counts_.value(key) >= 0;
}

qint64 AccountResourceView::count(const QString& key) const {
  return hasReliableCount(key) ? counts_.value(key) : 0;
}
