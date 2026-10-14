#pragma once
#include "contracts/public_data_contract.h"
#include <QJsonObject>
#include <utility>

// Frozen, pure-value access. No I/O, singleton, fuzzy name, numeric coercion,
// field deletion, or claim of a global publication revision (phase B owns it).
class PublicDataView final {
public:
  PublicDataView(PublicDataViewKind kind, QJsonObject root) : kind_(kind), root_(std::move(root)) {}
  PublicRecordValue record(const PublicRecordKey& key) const;
  QJsonObject object(const QString& table, const QString& id) const;
  QJsonValue table(const QString& name) const { return root_.value(name); }
  const QJsonObject& rawRoot() const { return root_; }
private:
  PublicDataViewKind kind_;
  QJsonObject root_;
};
