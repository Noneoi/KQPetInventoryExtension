#pragma once

#include <QHash>
#include <QDateTime>
#include <QString>

struct AccountResource {
  QString key;
  QString name;
  qint64 count = 0;
};

// Read-only, account-scoped view over resource counts already obtained by the
// existing shop/material cache. Absence always means unknown, never zero.
class AccountResourceView final {
public:
  AccountResourceView() = default;
  AccountResourceView(const QHash<QString, qint64>& counts,
                      bool sourceReliable, const QDateTime& observedAt = {},
                      const QString& source = {}, bool invalidated = false);

  bool hasReliableCount(const QString& key) const;
  qint64 count(const QString& key) const;
  bool sourceReliable() const { return sourceReliable_; }
  QDateTime observedAt() const { return observedAt_; }
  QString source() const { return source_; }
  bool invalidated() const { return invalidated_; }

private:
  QHash<QString, qint64> counts_;
  bool sourceReliable_ = false;
  QDateTime observedAt_;
  QString source_;
  bool invalidated_ = false;
};
