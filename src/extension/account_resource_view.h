#pragma once

#include <QHash>
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
                      bool sourceReliable);

  bool hasReliableCount(const QString& key) const;
  qint64 count(const QString& key) const;
  bool sourceReliable() const { return sourceReliable_; }

private:
  QHash<QString, qint64> counts_;
  bool sourceReliable_ = false;
};
