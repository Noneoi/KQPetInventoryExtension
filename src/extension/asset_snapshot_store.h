#pragma once

#include "asset_analysis_types.h"

#include <QString>

class PetRepository;

class AssetSnapshotStore final {
public:
  explicit AssetSnapshotStore(PetRepository* repository);

  bool write(const QString& account, const AccountAssetOverview& overview,
             QString* status) const;
  QList<AccountAssetSnapshot> readAll(const QString& account) const;

private:
  QString snapshotsDirectory() const;

  PetRepository* repository_ = nullptr;
};
