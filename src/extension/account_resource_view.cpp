#include "account_resource_view.h"

AccountResourceView::AccountResourceView(
    const QHash<QString, qint64>& counts, bool sourceReliable)
    : counts_(counts), sourceReliable_(sourceReliable) {}

bool AccountResourceView::hasReliableCount(const QString& key) const {
  return sourceReliable_ && counts_.contains(key);
}

qint64 AccountResourceView::count(const QString& key) const {
  return hasReliableCount(key) ? counts_.value(key) : 0;
}
